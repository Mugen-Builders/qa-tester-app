#include "qatester/ledger.h"

extern "C" {
#include <libcma/ledger.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qatester {

AssetRef AssetRef::ether() {
    return AssetRef{AssetKind::Base, Address{}, U256{}};
}
AssetRef AssetRef::erc20(const Address &token) {
    return AssetRef{AssetKind::Token, token, U256{}};
}
AssetRef AssetRef::erc721(const Address &token, const U256 &id) {
    return AssetRef{AssetKind::TokenId, token, id};
}
AssetRef AssetRef::erc1155(const Address &token, const U256 &id) {
    return AssetRef{AssetKind::TokenIdAmount, token, id};
}

AccountRef AccountRef::of_wallet(const Address &a) {
    AccountRef r;
    r.is_wallet = true;
    r.wallet = a;
    std::copy(a.b.begin(), a.b.end(), r.id.b.begin() + 12);
    return r;
}
AccountRef AccountRef::of_id(const U256 &id) {
    AccountRef r;
    r.is_wallet = false;
    r.id = id;
    return r;
}

struct Ledger::Impl {
    Config cfg;
    std::string path;
    cma_ledger_t single{};
    cma_ledger_t multi{};
    bool single_ok{false};
    bool multi_ok{false};
    cma_ledger_asset_id_t ether_id{0};
    uint8_t *drive_map{nullptr}; // view of the accounts drive prefix
    size_t drive_map_len{0};
    bool drive_map_owned{false}; // separate read-only mapping
    uint8_t *nvram{nullptr};     // non-null when the state path is a character device
    uint8_t *whole{nullptr};     // single-NVRAM layout: mapping of the whole range (region B in buffer mode)
    size_t whole_len{0};
    std::string err;

    static cma_ledger_asset_type_t kind_to_type(AssetKind k) {
        switch (k) {
            case AssetKind::Base:
                return CMA_LEDGER_ASSET_TYPE_BASE;
            case AssetKind::Token:
                return CMA_LEDGER_ASSET_TYPE_TOKEN_ADDRESS;
            case AssetKind::TokenId:
                return CMA_LEDGER_ASSET_TYPE_TOKEN_ADDRESS_ID;
            case AssetKind::TokenIdAmount:
                return CMA_LEDGER_ASSET_TYPE_TOKEN_ADDRESS_ID_AMOUNT;
        }
        return CMA_LEDGER_ASSET_TYPE_BASE;
    }

    [[nodiscard]] bool on_drive(const AssetRef &a) const {
        return a.kind == AssetKind::Token && a.token == cfg.token;
    }

    cma_ledger_t *select(const AssetRef &a) {
        return on_drive(a) ? &single : &multi;
    }

    int fail(const char *what, int code) {
        err = std::string(what) + ": " + cma_ledger_get_last_error_message();
        return code;
    }

    // Resolve (or create) the libcma asset id for `a` in the chosen ledger.
    int asset_id(cma_ledger_t *l, const AssetRef &a, cma_ledger_asset_id_t &out, bool create) {
        cma_ledger_asset_type_t type = kind_to_type(a.kind);
        cma_token_address_t token{};
        cma_token_id_t id{};
        std::copy(a.token.b.begin(), a.token.b.end(), std::begin(token.data));
        std::copy(a.id.b.begin(), a.id.b.end(), std::begin(id.data));
        cma_token_address_t *tp = (a.kind == AssetKind::Base) ? nullptr : &token;
        cma_token_id_t *ip = (a.kind == AssetKind::TokenId || a.kind == AssetKind::TokenIdAmount) ? &id : nullptr;
        if (l == &single) {
            out = 0; // the fixed single asset
            return 0;
        }
        if (a.kind == AssetKind::Base) {
            out = ether_id;
            return 0;
        }
        int rc = cma_ledger_retrieve_asset(l, &out, tp, ip, nullptr, &type,
            create ? CMA_LEDGER_OP_FIND_OR_CREATE : CMA_LEDGER_OP_FIND);
        if (rc != CMA_LEDGER_SUCCESS) {
            return fail("retrieve asset", rc);
        }
        return 0;
    }

    int account_id(cma_ledger_t *l, const AccountRef &acc, cma_ledger_account_id_t &out, bool create) {
        cma_ledger_account_type_t type =
            acc.is_wallet ? CMA_LEDGER_ACCOUNT_TYPE_WALLET_ADDRESS : CMA_LEDGER_ACCOUNT_TYPE_ACCOUNT_ID;
        const void *key = acc.is_wallet ? static_cast<const void *>(acc.wallet.b.data())
                                        : static_cast<const void *>(acc.id.b.data());
        int rc = cma_ledger_retrieve_account(l, &out, nullptr, key, nullptr, &type,
            create ? CMA_LEDGER_OP_FIND_OR_CREATE : CMA_LEDGER_OP_FIND);
        if (rc != CMA_LEDGER_SUCCESS) {
            return fail("retrieve account", rc);
        }
        return 0;
    }

    // Mirror of the sample app: drop accounts with no balances and assets with zero supply so
    // the ledger does not fill with empty entries (the single ledger compacts records itself).
    void prune_account(cma_ledger_t *l, cma_ledger_account_id_t id) {
        size_t n_balances = 0;
        cma_ledger_account_type_t type = CMA_LEDGER_ACCOUNT_TYPE_ID;
        if (cma_ledger_retrieve_account(l, &id, nullptr, nullptr, &n_balances, &type, CMA_LEDGER_OP_FIND) !=
            CMA_LEDGER_SUCCESS) {
            return;
        }
        if (n_balances == 0) {
            type = CMA_LEDGER_ACCOUNT_TYPE_ID;
            (void) cma_ledger_retrieve_account(l, &id, nullptr, nullptr, nullptr, &type, CMA_LEDGER_OP_FIND_AND_REMOVE);
        }
    }

    void prune_asset(cma_ledger_t *l, cma_ledger_asset_id_t id) {
        if (l == &single || id == ether_id) {
            return;
        }
        cma_amount_t supply{};
        cma_ledger_asset_type_t type = CMA_LEDGER_ASSET_TYPE_ID;
        if (cma_ledger_retrieve_asset(l, &id, nullptr, nullptr, &supply, &type, CMA_LEDGER_OP_FIND) !=
            CMA_LEDGER_SUCCESS) {
            return;
        }
        bool zero = std::all_of(std::begin(supply.data), std::end(supply.data), [](uint8_t x) { return x == 0; });
        if (zero) {
            type = CMA_LEDGER_ASSET_TYPE_ID;
            (void) cma_ledger_retrieve_asset(l, &id, nullptr, nullptr, nullptr, &type, CMA_LEDGER_OP_FIND_AND_REMOVE);
        }
    }
};

static cma_amount_t to_amount(const U256 &v) {
    cma_amount_t a{};
    std::copy(v.b.begin(), v.b.end(), std::begin(a.data));
    return a;
}

static void ensure_size(const std::string &path, size_t need) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::runtime_error("state file not found: " + path);
    }
    // A regular file used by host tests is grown here; devices (NVRAM /dev/uioN, flash /dev/pmemN)
    // have a fixed size that libcma cannot query and trusts.
    if (S_ISREG(st.st_mode) && static_cast<size_t>(st.st_size) < need) {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(need)) != 0) {
            if (fd >= 0) {
                ::close(fd);
            }
            throw std::runtime_error("cannot grow state file: " + path);
        }
        ::close(fd);
    }
}

Ledger::Ledger(const Config &cfg, const std::string &state_file, const std::string &ledger_file)
    : m(std::make_unique<Impl>()) {
    m->cfg = cfg;
    m->path = state_file;
    const bool same = ledger_file.empty() || ledger_file == state_file;
    struct stat st{};
    if (::stat(state_file.c_str(), &st) != 0) {
        throw std::runtime_error("state file not found: " + state_file);
    }
    const bool uio = S_ISCHR(st.st_mode);
    m->nvram = uio ? reinterpret_cast<uint8_t *>(1) : nullptr;

    ensure_size(state_file, same ? cfg.region_a_length + cfg.region_b_length : cfg.region_a_length);
    if (!same) {
        ensure_size(ledger_file, cfg.region_b_length);
    }

    // Region A (offset 0 of the state range): file-backed create-or-open, so a stored NVRAM image
    // keeps libcma's footer and account-driver-reader can read and prove it unchanged.
    cma_token_address_t token{};
    std::copy(cfg.token.b.begin(), cfg.token.b.end(), std::begin(token.data));
    int rc = cma_ledger_init_single_file(&m->single, state_file.c_str(), 0, cfg.region_a_length, cfg.single_accounts,
        CMA_LEDGER_ASSET_TYPE_TOKEN_ADDRESS, &token);
    if (rc != CMA_LEDGER_SUCCESS) {
        throw std::runtime_error(std::string("single ledger init: ") + cma_ledger_get_last_error_message());
    }
    m->single_ok = true;

    // Region B (multi-asset ledger).
    if (same && uio) {
        // One NVRAM for everything. A UIO device cannot be mmap'ed at a byte offset (the offset selects
        // a map index), so libcma's file mode cannot open the tail of the range: map the whole NVRAM
        // once and run the multi-asset ledger in libcma's buffer mode on the slice after region A.
        // Buffer mode has no footer, hence this half is not readable by account-driver-reader from a
        // stored image; the accounts drive (region A) is unaffected.
        int fd = ::open(state_file.c_str(), O_RDWR | O_SYNC);
        if (fd < 0) {
            throw std::runtime_error("cannot open NVRAM " + state_file + ": " + std::strerror(errno));
        }
        m->whole_len = cfg.region_a_length + cfg.region_b_length;
        void *p = ::mmap(nullptr, m->whole_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            m->whole_len = 0;
            throw std::runtime_error("cannot map NVRAM " + state_file + ": " + std::strerror(errno));
        }
        m->whole = static_cast<uint8_t *>(p);
        rc = cma_ledger_init_buffer(&m->multi, m->whole + cfg.region_a_length, cfg.region_b_length, cfg.multi_accounts,
            cfg.multi_assets, cfg.multi_balances);
    } else {
        const std::string &lpath = same ? state_file : ledger_file;
        rc = cma_ledger_init_file(&m->multi, lpath.c_str(), same ? cfg.region_a_length : 0, cfg.region_b_length,
            cfg.multi_accounts, cfg.multi_assets, cfg.multi_balances);
    }
    if (rc != CMA_LEDGER_SUCCESS) {
        throw std::runtime_error(std::string("multi ledger init: ") + cma_ledger_get_last_error_message());
    }
    m->multi_ok = true;

    cma_ledger_asset_type_t base = CMA_LEDGER_ASSET_TYPE_BASE;
    rc = cma_ledger_retrieve_asset(&m->multi, &m->ether_id, nullptr, nullptr, nullptr, &base,
        CMA_LEDGER_OP_FIND_OR_CREATE);
    if (rc != CMA_LEDGER_SUCCESS) {
        throw std::runtime_error(std::string("ether asset: ") + cma_ledger_get_last_error_message());
    }

    // View of the accounts drive prefix (offset 0 of the state range), for state reports.
    if (m->whole != nullptr) {
        m->drive_map = m->whole;
        m->drive_map_len = cfg.accounts_drive_length();
    } else {
        int fd = ::open(state_file.c_str(), O_RDONLY);
        if (fd >= 0) {
            m->drive_map_len = cfg.accounts_drive_length();
            void *p = ::mmap(nullptr, m->drive_map_len, PROT_READ, MAP_SHARED, fd, 0);
            ::close(fd);
            if (p != MAP_FAILED) {
                m->drive_map = static_cast<uint8_t *>(p);
                m->drive_map_owned = true;
            } else {
                m->drive_map_len = 0;
            }
        }
    }
}

Ledger::~Ledger() {
    if (m->multi_ok) {
        (void) cma_ledger_fini(&m->multi);
    }
    if (m->single_ok) {
        (void) cma_ledger_fini(&m->single);
    }
    if (m->drive_map_owned && m->drive_map != nullptr) {
        ::munmap(m->drive_map, m->drive_map_len);
    }
    if (m->whole != nullptr) {
        ::munmap(m->whole, m->whole_len);
    }
}

bool Ledger::is_accounts_drive_asset(const AssetRef &a) const {
    return m->on_drive(a);
}

int Ledger::deposit(const AccountRef &to, const AssetRef &asset, const U256 &amount) {
    cma_ledger_t *l = m->select(asset);
    cma_ledger_asset_id_t aid = 0;
    cma_ledger_account_id_t acc = 0;
    int rc = m->asset_id(l, asset, aid, true);
    if (rc != 0) {
        return rc;
    }
    rc = m->account_id(l, to, acc, true);
    if (rc != 0) {
        return rc;
    }
    cma_amount_t a = to_amount(amount);
    rc = cma_ledger_deposit(l, aid, acc, &a);
    if (rc != CMA_LEDGER_SUCCESS) {
        m->fail("deposit", rc);
        m->prune_account(l, acc);
        m->prune_asset(l, aid);
        return rc;
    }
    return 0;
}

int Ledger::withdraw(const AccountRef &from, const AssetRef &asset, const U256 &amount) {
    cma_ledger_t *l = m->select(asset);
    cma_ledger_asset_id_t aid = 0;
    cma_ledger_account_id_t acc = 0;
    int rc = m->asset_id(l, asset, aid, false);
    if (rc != 0) {
        return rc;
    }
    rc = m->account_id(l, from, acc, false);
    if (rc != 0) {
        return rc;
    }
    cma_amount_t a = to_amount(amount);
    rc = cma_ledger_withdraw(l, aid, acc, &a);
    if (rc != CMA_LEDGER_SUCCESS) {
        return m->fail("withdraw", rc);
    }
    m->prune_account(l, acc);
    m->prune_asset(l, aid);
    return 0;
}

int Ledger::transfer(const AccountRef &from, const AccountRef &to, const AssetRef &asset, const U256 &amount) {
    cma_ledger_t *l = m->select(asset);
    cma_ledger_asset_id_t aid = 0;
    cma_ledger_account_id_t src = 0;
    cma_ledger_account_id_t dst = 0;
    int rc = m->asset_id(l, asset, aid, false);
    if (rc != 0) {
        return rc;
    }
    rc = m->account_id(l, from, src, false);
    if (rc != 0) {
        return rc;
    }
    rc = m->account_id(l, to, dst, true);
    if (rc != 0) {
        return rc;
    }
    cma_amount_t a = to_amount(amount);
    rc = cma_ledger_transfer(l, aid, src, dst, &a);
    if (rc != CMA_LEDGER_SUCCESS) {
        m->fail("transfer", rc);
        m->prune_account(l, dst);
        return rc;
    }
    m->prune_account(l, src);
    return 0;
}

int Ledger::balance(const AccountRef &account, const AssetRef &asset, U256 &out) {
    out = U256{};
    cma_ledger_t *l = m->select(asset);
    cma_ledger_asset_id_t aid = 0;
    cma_ledger_account_id_t acc = 0;
    int rc = m->asset_id(l, asset, aid, false);
    if (rc != 0) {
        return rc;
    }
    rc = m->account_id(l, account, acc, false);
    if (rc != 0) {
        return rc;
    }
    cma_amount_t b{};
    rc = cma_ledger_get_balance(l, aid, acc, &b, nullptr);
    if (rc != CMA_LEDGER_SUCCESS) {
        return m->fail("get balance", rc);
    }
    std::copy(std::begin(b.data), std::end(b.data), out.b.begin());
    return 0;
}

int Ledger::total_supply(const AssetRef &asset, U256 &out) {
    out = U256{};
    cma_ledger_t *l = m->select(asset);
    cma_ledger_asset_id_t aid = 0;
    int rc = m->asset_id(l, asset, aid, false);
    if (rc != 0) {
        return rc;
    }
    cma_amount_t supply{};
    cma_ledger_asset_type_t type = CMA_LEDGER_ASSET_TYPE_ID;
    rc = cma_ledger_retrieve_asset(l, &aid, nullptr, nullptr, &supply, &type, CMA_LEDGER_OP_FIND);
    if (rc != CMA_LEDGER_SUCCESS) {
        return m->fail("total supply", rc);
    }
    std::copy(std::begin(supply.data), std::end(supply.data), out.b.begin());
    return 0;
}

std::string Ledger::last_error() const {
    return m->err;
}

bytes_view Ledger::accounts_drive() const {
    return bytes_view(m->drive_map, m->drive_map_len);
}

bool Ledger::is_nvram() const {
    return m->nvram != nullptr;
}

void Ledger::flush() {
    // libcma syncs its mapping after every mutating call; an NVRAM has no page cache anyway.
}

} // namespace qatester
