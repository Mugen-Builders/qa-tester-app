// Ledger facade over libcma: one state range (in the machine the `state` NVRAM, /dev/uio0) holding
//   region A [0, region_a_length): single-asset ledger for the configured ERC-20, file-backed
//            (create-or-open, libcma footer). Its first single_accounts*32 bytes are the accounts
//            drive (uint96 LE balance + address records) that rollups-contracts v3 proves for
//            emergency withdrawals, and libcma's account-driver-reader reads from a stored image.
//   region B [region_a_length, +region_b_length): multi-asset ledger for every other asset
//            (Ether, other ERC-20s, ERC-721, ERC-1155). File-backed when the range is a file or a
//            flash drive; buffer mode over a mapping of the whole NVRAM when it is a UIO device
//            (UIO cannot be mapped at a byte offset).
// A second path may be given to put region B in another range instead.
#pragma once

#include "config.h"
#include "types.h"

#include <memory>
#include <string>

namespace qatester {

enum class AssetKind : uint8_t { Base, Token, TokenId, TokenIdAmount };

struct AssetRef {
    AssetKind kind{AssetKind::Base};
    Address token;
    U256 id;
    [[nodiscard]] static AssetRef ether();
    [[nodiscard]] static AssetRef erc20(const Address &token);
    [[nodiscard]] static AssetRef erc721(const Address &token, const U256 &id);
    [[nodiscard]] static AssetRef erc1155(const Address &token, const U256 &id);
};

// 32-byte account identifier: a wallet address is left-padded with 12 zero bytes.
struct AccountRef {
    bool is_wallet{true};
    Address wallet;
    U256 id;
    [[nodiscard]] static AccountRef of_wallet(const Address &a);
    [[nodiscard]] static AccountRef of_id(const U256 &id);
};

class Ledger {
public:
    // create-or-open; throws std::runtime_error. `ledger_file` empty or equal to `state_file` puts the
    // multi-asset region in the same file after the state region.
    Ledger(const Config &cfg, const std::string &state_file, const std::string &ledger_file = "");
    ~Ledger();
    Ledger(const Ledger &) = delete;
    Ledger &operator=(const Ledger &) = delete;

    [[nodiscard]] bool is_accounts_drive_asset(const AssetRef &a) const; // configured token
    // All return 0 or a negative libcma error code; `last_error()` has the message.
    int deposit(const AccountRef &to, const AssetRef &asset, const U256 &amount);
    int withdraw(const AccountRef &from, const AssetRef &asset, const U256 &amount);
    int transfer(const AccountRef &from, const AccountRef &to, const AssetRef &asset, const U256 &amount);
    int balance(const AccountRef &account, const AssetRef &asset, U256 &out);
    int total_supply(const AssetRef &asset, U256 &out);
    [[nodiscard]] std::string last_error() const;

    [[nodiscard]] bytes_view accounts_drive() const; // the proven prefix of region A
    [[nodiscard]] bool is_nvram() const;             // state path is a character device (UIO NVRAM)
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace qatester
