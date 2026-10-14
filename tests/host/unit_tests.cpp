// Host unit tests for libqatester (no framework; each check aborts with a message on failure).
#include "qatester/abi.h"
#include "qatester/commands.h"
#include "qatester/config.h"
#include "qatester/engine.h"
#include "qatester/fee.h"
#include "qatester/ledger.h"
#include "qatester/seq_ext.h"
#include "qatester/state.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/stat.h>

using namespace qatester;

static int g_checks = 0;
#define CHECK(cond)                                                                                        \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        if (!(cond)) {                                                                                     \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            std::exit(1);                                                                                  \
        }                                                                                                  \
    } while (0)

namespace {

Address addr(uint8_t last) {
    Address a;
    a.b[19] = last;
    return a;
}

struct Emission {
    std::string kind;
    Address dest;
    U256 value;
    bytes payload;
};

// Records everything; can be told to fail after N outputs (to emulate -ENOBUFS).
struct RecordingSink : OutputSink {
    std::vector<Emission> items;
    int fail_after{-1};
    int seen{0};
    int next() {
        if (fail_after >= 0 && seen >= fail_after) {
            return -ENOBUFS;
        }
        ++seen;
        return 0;
    }
    int notice(bytes_view p) override {
        int rc = next();
        if (rc == 0) {
            items.push_back({"notice", {}, {}, bytes(p.begin(), p.end())});
        }
        return rc;
    }
    int voucher(const Address &d, const U256 &v, bytes_view p) override {
        int rc = next();
        if (rc == 0) {
            items.push_back({"voucher", d, v, bytes(p.begin(), p.end())});
        }
        return rc;
    }
    int delegate_call_voucher(const Address &d, bytes_view p) override {
        int rc = next();
        if (rc == 0) {
            items.push_back({"delegate", d, {}, bytes(p.begin(), p.end())});
        }
        return rc;
    }
    int raw_output(bytes_view p) override {
        int rc = next();
        if (rc == 0) {
            items.push_back({"raw", {}, {}, bytes(p.begin(), p.end())});
        }
        return rc;
    }
    int report(bytes_view p) override {
        items.push_back({"report", {}, {}, bytes(p.begin(), p.end())});
        return 0;
    }
    void progress(uint32_t v) override {
        items.push_back({"progress", {}, U256::from_u64(v), {}});
    }
    size_t count(const char *kind) const {
        size_t n = 0;
        for (auto &e : items) {
            n += e.kind == kind;
        }
        return n;
    }
    std::string last_report() const {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->kind == "report") {
                return std::string(it->payload.begin(), it->payload.end());
            }
        }
        return "";
    }
};

bytes call(Cmd cmd, auto fill) {
    abi::Writer w;
    w.selector(selector_of(cmd));
    fill(w);
    return w.finish();
}

DirectInput input(const Address &sender, bytes_view payload, uint64_t index = 0) {
    DirectInput in;
    in.sender = sender;
    in.payload = payload;
    in.index = index;
    in.block_number = 100 + index;
    in.app_contract = addr(0xAA);
    return in;
}

bytes read_file(const std::string &path);

// Executes like the classic driver: accepted inputs advance the progress, rejected ones are counted.
Completion drive(Engine &eng, const DirectInput &in, OutputSink &sink, TerminalArgs &term) {
    Completion c = eng.execute_direct_input(in, sink, term);
    if (c == Completion::Accept) {
        eng.commit_progress(in.block_number);
    } else if (c == Completion::Reject) {
        eng.note_reject();
    }
    return c;
}

std::string temp_state_file() {
    char tmpl[] = "/tmp/qa-tester-state-XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    close(fd);
    return tmpl;
}

Config test_config() {
    Config c = Config::devnet_defaults();
    c.log = false;
    c.single_accounts = 1024;
    c.region_a_length = 2u << 20;
    c.region_b_length = 2u << 20;
    c.region_c_length = 64u << 10; // meta page + 1920 nonce slots
    c.multi_accounts = 256;
    c.multi_assets = 16;
    c.multi_balances = 1024;
    c.safe_erc20_transfer = addr(0x55);
    return c;
}

// --------------------------------------------------------------------------------------------
void test_selectors_match_cast() {
    struct {
        Cmd cmd;
        uint32_t sel;
    } expected[] = {
        {Cmd::EmitNotices, 0x3fd67492}, {Cmd::EmitNotice, 0x3e185366}, {Cmd::EmitReports, 0xc4ec7ed6},
        {Cmd::EmitReport, 0x3acd1443}, {Cmd::EmitVouchers, 0x4982adac}, {Cmd::EmitDelegateCallVoucher, 0x0bcf387c},
        {Cmd::EmitRawOutput, 0xa91adbb7}, {Cmd::Echo, 0x624fbfdc}, {Cmd::Batch, 0x1e897afb}, {Cmd::Reject, 0x70f4c930},
        {Cmd::RaiseException, 0xeb7b4d7f}, {Cmd::Halt, 0x6abdfe32}, {Cmd::ManualYield, 0xda183923},
        {Cmd::CorruptOutputsRoot, 0x2a3387c5}, {Cmd::BurnCycles, 0xe2b618e5}, {Cmd::Crash, 0xb2914ced},
        {Cmd::SetDepositPolicy, 0xf62c402a}, {Cmd::WithdrawErc20Safe, 0xa24639f5}, {Cmd::Progress, 0xe2a579c6},
    };
    for (auto &e : expected) {
        CHECK(selector_of(e.cmd) == e.sel);
    }
    CHECK(abi::selector("safeTransfer(address,address,uint256)") == 0xd1660f99);
    CHECK(abi::selector("Voucher(address,uint256,bytes)") == 0x237a816f);
}

void test_abi_roundtrip() {
    bytes payload = {1, 2, 3, 4, 5};
    bytes enc = call(Cmd::EmitVouchers, [&](abi::Writer &w) {
        w.uint(3);
        w.address(addr(0x42));
        w.word(U256::from_u64(7));
        w.dyn_bytes(payload);
    });
    CHECK(enc.size() == 4 + 4 * 32 + 32 + 32); // head + length + padded data
    Command c;
    std::string err;
    CHECK(decode_command(enc, c, err) == DecodeStatus::Ok);
    CHECK(c.cmd == Cmd::EmitVouchers && c.count == 3 && c.addr == addr(0x42) && c.value.to_u64() == 7);
    CHECK(bytes(c.data.begin(), c.data.end()) == payload);

    // malformed: truncated head
    bytes cut(enc.begin(), enc.begin() + 40);
    CHECK(decode_command(cut, c, err) == DecodeStatus::Malformed);
    // malformed: bytes length past the end
    bytes bad = enc;
    bad[4 + 4 * 32 + 31] = 0xff;
    CHECK(decode_command(bad, c, err) == DecodeStatus::Malformed);
    // unknown selector / empty
    bytes unk = {0xde, 0xad, 0xbe, 0xef};
    CHECK(decode_command(unk, c, err) == DecodeStatus::Unknown);
    CHECK(decode_command(bytes{}, c, err) == DecodeStatus::Empty);
    // uint8 range check
    bytes halt = call(Cmd::Halt, [&](abi::Writer &w) { w.uint(300); });
    CHECK(decode_command(halt, c, err) == DecodeStatus::Malformed);

    // batch of two calls
    bytes a = call(Cmd::EmitReport, [&](abi::Writer &w) { w.dyn_bytes(bytes{9}); });
    bytes b = call(Cmd::Halt, [&](abi::Writer &w) { w.uint(1); });
    abi::Writer w;
    w.selector(selector_of(Cmd::Batch));
    // bytes[] layout by hand: offset word, then [len][off0][off1][len a][a][len b][b]
    bytes arr;
    auto push_word = [&](uint64_t v) {
        U256 x = U256::from_u64(v);
        arr.insert(arr.end(), x.b.begin(), x.b.end());
    };
    auto padded = [](const bytes &d) {
        bytes out;
        U256 l = U256::from_u64(d.size());
        out.insert(out.end(), l.b.begin(), l.b.end());
        out.insert(out.end(), d.begin(), d.end());
        out.insert(out.end(), (32 - d.size() % 32) % 32, 0);
        return out;
    };
    push_word(2);
    push_word(64);
    push_word(64 + padded(a).size());
    bytes pa = padded(a), pb = padded(b);
    arr.insert(arr.end(), pa.begin(), pa.end());
    arr.insert(arr.end(), pb.begin(), pb.end());
    bytes batch = {0x1e, 0x89, 0x7a, 0xfb};
    U256 off = U256::from_u64(32);
    batch.insert(batch.end(), off.b.begin(), off.b.end());
    batch.insert(batch.end(), arr.begin(), arr.end());
    CHECK(decode_command(batch, c, err) == DecodeStatus::Ok);
    CHECK(c.cmd == Cmd::Batch && c.calls.size() == 2);
    CHECK(bytes(c.calls[0].begin(), c.calls[0].end()) == a);
    CHECK(bytes(c.calls[1].begin(), c.calls[1].end()) == b);
}

void test_u256_text() {
    U256 v;
    CHECK(parse_u256("1000000", v) && v.to_u64() == 1000000);
    CHECK(parse_u256("0xff", v) && v.to_u64() == 255);
    CHECK(u256_to_dec(U256::from_u64(123456789)) == "123456789");
    CHECK(u256_to_dec(U256{}) == "0");
    CHECK(!parse_u256("12x", v));
    Address a;
    CHECK(parse_address("0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81", a) && a.b[0] == 0x7a && a.b[19] == 0x81);
    CHECK(!parse_address("0x1234", a));
}

void test_ledger_routing_and_records() {
    Config cfg = test_config();
    std::string path = temp_state_file();
    {
        Ledger l(cfg, path);
        Address u1 = addr(0x11);
        Address u2 = addr(0x22);
        AssetRef usd = AssetRef::erc20(cfg.token);
        AssetRef other = AssetRef::erc20(addr(0x99));
        CHECK(l.is_accounts_drive_asset(usd) && !l.is_accounts_drive_asset(other) && !l.is_accounts_drive_asset(AssetRef::ether()));

        CHECK(l.deposit(AccountRef::of_wallet(u1), usd, U256::from_u64(1000)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u2), usd, U256::from_u64(5)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u1), other, U256::from_u64(77)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u1), AssetRef::ether(), U256::from_u64(3)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u1), AssetRef::erc721(addr(0x33), U256::from_u64(9)), U256::from_u64(1)) == 0);

        // record 0 = u1 with 1000 (uint96 little-endian) + address, as LibUsdAccount.encode produces
        bytes_view drive = l.accounts_drive();
        CHECK(drive.size() == cfg.accounts_drive_length());
        CHECK(drive[0] == 0xe8 && drive[1] == 0x03 && drive[2] == 0 && drive[11] == 0);
        CHECK(std::equal(u1.b.begin(), u1.b.end(), drive.begin() + 12));
        CHECK(drive[32] == 5 && std::equal(u2.b.begin(), u2.b.end(), drive.begin() + 32 + 12));
        // the "other" token never touches the drive
        for (size_t i = 64; i < 96; ++i) {
            CHECK(drive[i] == 0);
        }

        U256 bal;
        CHECK(l.balance(AccountRef::of_wallet(u1), usd, bal) == 0 && bal.to_u64() == 1000);
        CHECK(l.balance(AccountRef::of_wallet(u1), other, bal) == 0 && bal.to_u64() == 77);
        CHECK(l.balance(AccountRef::of_wallet(u1), AssetRef::ether(), bal) == 0 && bal.to_u64() == 3);
        CHECK(l.total_supply(usd, bal) == 0 && bal.to_u64() == 1005);

        // overdraft is refused and leaves the balance untouched
        CHECK(l.withdraw(AccountRef::of_wallet(u1), usd, U256::from_u64(1001)) != 0);
        CHECK(l.balance(AccountRef::of_wallet(u1), usd, bal) == 0 && bal.to_u64() == 1000);
        // withdraw everything: the record is released (no stale non-empty record with zero balance)
        CHECK(l.withdraw(AccountRef::of_wallet(u1), usd, U256::from_u64(1000)) == 0);
        bool rec_u1_gone = true;
        for (size_t r = 0; r < 4; ++r) {
            bytes_view rec = drive.subspan(r * 32, 32);
            bool zero_balance = std::all_of(rec.begin(), rec.begin() + 12, [](uint8_t x) { return x == 0; });
            bool is_u1 = std::equal(u1.b.begin(), u1.b.end(), rec.begin() + 12);
            if (is_u1 && zero_balance) {
                rec_u1_gone = false;
            }
            if (zero_balance) { // an empty record must be fully zero (address included)
                CHECK(std::all_of(rec.begin(), rec.end(), [](uint8_t x) { return x == 0; }));
            }
        }
        CHECK(rec_u1_gone);
        CHECK(l.balance(AccountRef::of_wallet(u2), usd, bal) == 0 && bal.to_u64() == 5);

        // uint96 overflow on the accounts drive is refused
        U256 big;
        big.b[19] = 1; // 2^96
        CHECK(l.deposit(AccountRef::of_wallet(u2), usd, big) != 0);
        CHECK(l.balance(AccountRef::of_wallet(u2), usd, bal) == 0 && bal.to_u64() == 5);
        // but the multi-asset ledger takes full 256-bit amounts
        CHECK(l.deposit(AccountRef::of_wallet(u2), other, big) == 0);

        // transfer to a virtual (32-byte id) account and back
        U256 vid;
        vid.b[0] = 0xcc;
        CHECK(l.transfer(AccountRef::of_wallet(u2), AccountRef::of_id(vid), usd, U256::from_u64(2)) == 0);
        CHECK(l.balance(AccountRef::of_id(vid), usd, bal) == 0 && bal.to_u64() == 2);
        CHECK(l.balance(AccountRef::of_wallet(u2), usd, bal) == 0 && bal.to_u64() == 3);
    }
    {
        // reopen: state persisted in the file
        Ledger l(cfg, path);
        U256 bal;
        CHECK(l.balance(AccountRef::of_wallet(addr(0x22)), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 3);
        CHECK(l.balance(AccountRef::of_wallet(addr(0x11)), AssetRef::erc20(addr(0x99)), bal) == 0 && bal.to_u64() == 77);
    }
    unlink(path.c_str());
}

bytes packed_erc20_deposit(const Address &token, const Address &sender, uint64_t amount, bytes_view exec) {
    bytes p;
    p.insert(p.end(), token.b.begin(), token.b.end());
    p.insert(p.end(), sender.b.begin(), sender.b.end());
    U256 a = U256::from_u64(amount);
    p.insert(p.end(), a.b.begin(), a.b.end());
    p.insert(p.end(), exec.begin(), exec.end());
    return p;
}

void test_engine_flow() {
    Config cfg = test_config();
    std::string path = temp_state_file();
    CHECK(!Engine::is_initialized(cfg, path));
    bool refused = false;
    try {
        Engine bad(cfg, path);
    } catch (const OpenError &e) {
        refused = e.kind == OpenErrorKind::InvalidDump;
    }
    CHECK(refused);
    Engine::genesis(cfg, path, addr(0x5e));
    CHECK(Engine::is_initialized(cfg, path));
    Engine eng(cfg, path);
    CHECK(eng.sequencer() == addr(0x5e));
    Address user = addr(0x77);
    TerminalArgs term;

    // 1. ERC-20 deposit of the configured token via the portal
    {
        RecordingSink s;
        bytes exec = {0xde, 0xad};
        bytes p = packed_erc20_deposit(cfg.token, user, 1000000, exec);
        CHECK(drive(eng, input(cfg.erc20_portal, p, 0), s, term) == Completion::Accept);
        CHECK(s.count("report") == 1);
        CHECK(s.last_report().find("\"credited\":true") != std::string::npos);
        CHECK(s.last_report().find("\"exec_layer_data\":\"0xdead\"") != std::string::npos);
        CHECK(s.last_report().find("\"accounts_drive\":true") != std::string::npos);
    }
    // 2. a deposit of another token is credited too, but not on the accounts drive
    {
        RecordingSink s;
        bytes p = packed_erc20_deposit(addr(0x99), user, 50, {});
        CHECK(drive(eng, input(cfg.erc20_portal, p, 1), s, term) == Completion::Accept);
        CHECK(s.last_report().find("\"accounts_drive\":false") != std::string::npos);
    }
    // 3. malformed deposit payload (too short) -> reject with a report
    {
        RecordingSink s;
        bytes p = {1, 2, 3};
        CHECK(drive(eng, input(cfg.erc20_portal, p, 2), s, term) == Completion::Reject);
        CHECK(s.last_report().find("error") != std::string::npos);
    }
    // 4. plain ERC-20 withdrawal through libcma -> Voucher(token.transfer(user, amount))
    {
        RecordingSink s;
        abi::Writer w;
        w.selector(0x4f94d342); // WithdrawErc20(address,uint256,bytes)
        w.address(cfg.token);
        w.word(U256::from_u64(400000));
        w.dyn_bytes(bytes{});
        bytes p = w.finish();
        CHECK(drive(eng, input(user, p, 3), s, term) == Completion::Accept);
        CHECK(s.count("voucher") == 1);
        const Emission &v = s.items.back();
        CHECK(v.dest == cfg.token && v.value.is_zero());
        CHECK(v.payload.size() == 4 + 64);
        CHECK(v.payload[0] == 0xa9 && v.payload[1] == 0x05 && v.payload[2] == 0x9c && v.payload[3] == 0xbb);
        CHECK(std::equal(user.b.begin(), user.b.end(), v.payload.begin() + 4 + 12));
        CHECK(v.payload[4 + 32 + 31] == (400000 & 0xff));
    }
    // 5. safe withdrawal -> DelegateCallVoucher(safeErc20Transfer, safeTransfer(token, user, amount))
    {
        RecordingSink s;
        bytes p = call(Cmd::WithdrawErc20Safe, [&](abi::Writer &w) {
            w.address(Address{}); // use the configured default
            w.address(cfg.token);
            w.word(U256::from_u64(100000));
            w.dyn_bytes(bytes{});
        });
        CHECK(drive(eng, input(user, p, 4), s, term) == Completion::Accept);
        CHECK(s.count("delegate") == 1);
        const Emission &v = s.items.back();
        CHECK(v.dest == addr(0x55));
        CHECK(v.payload.size() == 4 + 96);
        CHECK(v.payload[0] == 0xd1 && v.payload[1] == 0x66 && v.payload[2] == 0x0f && v.payload[3] == 0x99);
        CHECK(std::equal(cfg.token.b.begin(), cfg.token.b.end(), v.payload.begin() + 4 + 12));
        CHECK(std::equal(user.b.begin(), user.b.end(), v.payload.begin() + 4 + 32 + 12));
    }
    // 6. overdraft -> reject, balance intact (1000000 - 400000 - 100000 = 500000)
    {
        RecordingSink s;
        bytes p = call(Cmd::WithdrawErc20Safe, [&](abi::Writer &w) {
            w.address(Address{});
            w.address(cfg.token);
            w.word(U256::from_u64(500001));
            w.dyn_bytes(bytes{});
        });
        CHECK(drive(eng, input(user, p, 5), s, term) == Completion::Reject);
        U256 bal;
        CHECK(eng.ledger().balance(AccountRef::of_wallet(user), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 500000);
    }
    // 7. inspect balance via libcma JSON and raw "state"
    {
        RecordingSink s;
        std::string q = "{\"method\":\"ledger_getBalance\",\"params\":[\"" + to_hex(user) + "\",\"" + to_hex(cfg.token) + "\"]}";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(q.data()), q.size()), s, term) == Completion::Accept);
        CHECK(s.count("report") == 1 && s.items.back().payload.size() == 32);
        U256 got;
        std::copy(s.items.back().payload.begin(), s.items.back().payload.end(), got.b.begin());
        CHECK(got.to_u64() == 500000);
        RecordingSink s2;
        const char *st = "state";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(st), 5), s2, term) == Completion::Accept);
        CHECK(s2.last_report().find("\"executed_input_count\":4") != std::string::npos);
        RecordingSink s3;
        bytes raw(100, 0xab);
        CHECK(eng.inspect(raw, s3, term) == Completion::Accept && s3.items.back().payload == raw);
        RecordingSink s4;
        std::string e = "{\"method\":\"qa_exception\",\"params\":[\"0x0102\"]}";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(e.data()), e.size()), s4, term) == Completion::Exception);
        CHECK(term.payload == bytes({1, 2}));
    }
    // 8. emitNotices with the exact OUT-002 pattern and count; sink failure -> reject + error report
    {
        RecordingSink s;
        bytes p = call(Cmd::EmitNotices, [&](abi::Writer &w) {
            w.uint(3);
            w.uint(300);
            w.uint(0);
        });
        CHECK(drive(eng, input(user, p, 6), s, term) == Completion::Accept);
        CHECK(s.count("notice") == 3 && s.items[0].payload.size() == 300 && s.items[0].payload[255] == 0xff && s.items[0].payload[256] == 0);
        RecordingSink f;
        f.fail_after = 1;
        CHECK(drive(eng, input(user, p, 7), f, term) == Completion::Reject);
        CHECK(f.count("notice") == 1 && f.last_report().find("unable to emit notice #1") != std::string::npos);
        CHECK(f.last_report().find("\"code\":-105") != std::string::npos);
    }
    // 9. batch: report, voucher, then corrupt root -> completion propagates, earlier emissions kept
    {
        RecordingSink s;
        bytes a = call(Cmd::EmitReport, [&](abi::Writer &w) { w.dyn_bytes(bytes{1}); });
        bytes b = call(Cmd::EmitVouchers, [&](abi::Writer &w) {
            w.uint(1);
            w.address(addr(0x42));
            w.word(U256{});
            w.dyn_bytes(bytes{});
        });
        bytes c = call(Cmd::CorruptOutputsRoot, [&](abi::Writer &w) { w.uint(0); });
        bytes d = call(Cmd::EmitNotice, [&](abi::Writer &w) { w.dyn_bytes(bytes{2}); }); // never reached
        // encode batch(bytes[]) through the writer's dyn_bytes is not supported; build by hand
        abi::Writer w;
        w.selector(selector_of(Cmd::Batch));
        w.uint(32);
        bytes head = w.finish();
        bytes arr;
        auto wordp = [&](uint64_t v) {
            U256 x = U256::from_u64(v);
            arr.insert(arr.end(), x.b.begin(), x.b.end());
        };
        std::vector<bytes> items = {a, b, c, d};
        wordp(items.size());
        size_t off = 32 * items.size();
        std::vector<bytes> padded;
        for (auto &it : items) {
            wordp(off);
            bytes pd;
            U256 l = U256::from_u64(it.size());
            pd.insert(pd.end(), l.b.begin(), l.b.end());
            pd.insert(pd.end(), it.begin(), it.end());
            pd.insert(pd.end(), (32 - it.size() % 32) % 32, 0);
            off += pd.size();
            padded.push_back(pd);
        }
        for (auto &pd : padded) {
            arr.insert(arr.end(), pd.begin(), pd.end());
        }
        head.insert(head.end(), arr.begin(), arr.end());
        CHECK(drive(eng, input(user, head, 8), s, term) == Completion::CorruptRootValue);
        CHECK(s.count("report") == 1 && s.count("voucher") == 1 && s.count("notice") == 0);
    }
    // 10. terminal commands carry their arguments
    {
        RecordingSink s;
        bytes p = call(Cmd::ManualYield, [&](abi::Writer &w) {
            w.uint(8);
            w.uint(5);
        });
        CHECK(drive(eng, input(user, p, 9), s, term) == Completion::UnexpectedYield);
        CHECK(term.yield_reason == 8 && term.yield_data == 5);
        bytes h = call(Cmd::Halt, [&](abi::Writer &w) { w.uint(2); });
        CHECK(drive(eng, input(user, h, 10), s, term) == Completion::Halt && term.mode == 2);
        bytes e = call(Cmd::RaiseException, [&](abi::Writer &w) { w.dyn_bytes(bytes{'x', 'y'}); });
        CHECK(drive(eng, input(user, e, 11), s, term) == Completion::Exception && term.payload == bytes({'x', 'y'}));
        bytes u = {0xde, 0xad, 0xbe, 0xef, 0x00};
        CHECK(drive(eng, input(user, u, 12), s, term) == Completion::Reject);
        CHECK(s.last_report().find("unknown input") != std::string::npos);
        CHECK(drive(eng, input(user, bytes{}, 13), s, term) == Completion::Reject);
        CHECK(s.last_report().find("empty payload") != std::string::npos);
    }
    // 11. deposit policy switch
    {
        RecordingSink s;
        bytes p = call(Cmd::SetDepositPolicy, [&](abi::Writer &w) { w.uint(1); });
        CHECK(drive(eng, input(user, p, 14), s, term) == Completion::Accept);
        bytes dep = packed_erc20_deposit(addr(0x99), user, 1, {});
        CHECK(drive(eng, input(cfg.erc20_portal, dep, 15), s, term) == Completion::Reject);
        CHECK(s.last_report().find("deposit policy") != std::string::npos);
        bytes dep2 = packed_erc20_deposit(cfg.token, user, 1, {});
        CHECK(drive(eng, input(cfg.erc20_portal, dep2, 16), s, term) == Completion::Accept);
    }
    // 12. progress counters: accepted inputs only, persisted in the meta page
    CHECK(eng.progress().executed_input_count == 7);
    CHECK(eng.progress().last_block == 116);
    CHECK(eng.rejected_count() == 6);
    {
        Engine again(cfg, path);
        CHECK(again.progress().executed_input_count == 7 && again.progress().last_block == 116);
        CHECK(again.rejected_count() == 6);
        CHECK(again.config().deposit_policy == DepositPolicy::RejectNonToken); // setDepositPolicy persisted
        CHECK(again.sequencer() == addr(0x5e) && again.token() == cfg.token);
    }
    {
        // the pinned token wins over the environment/config on reopen
        Config other = cfg;
        other.token = addr(0x70);
        Engine again(other, path);
        CHECK(again.token() == cfg.token && again.config().token == cfg.token);
        U256 bal;
        CHECK(again.ledger().balance(AccountRef::of_wallet(user), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 500001);
    }
    unlink(path.c_str());
}

void test_genesis_pins_a_configured_token() {
    Config cfg = test_config();
    std::string path = temp_state_file();
    Address custom = addr(0x70);
    setenv("QA_LOG", "0", 1);
    setenv("QA_SINGLE_ACCOUNTS", "1024", 1);
    setenv("QA_REGION_A_LENGTH", std::to_string(cfg.region_a_length).c_str(), 1);
    setenv("QA_REGION_B_LENGTH", std::to_string(cfg.region_b_length).c_str(), 1);
    setenv("QA_REGION_C_LENGTH", std::to_string(cfg.region_c_length).c_str(), 1);
    setenv("QA_MULTI_ACCOUNTS", "256", 1);
    setenv("QA_MULTI_ASSETS", "16", 1);
    setenv("QA_MULTI_BALANCES", "1024", 1);
    unlink(path.c_str());
    CHECK(qatester_genesis(path.c_str(), addr(0x5e).b.data(), custom.b.data()) == 0);
    Engine eng(cfg, path); // cfg still names the devnet token: the pinned one replaces it
    CHECK(eng.token() == custom && eng.config().token == custom);
    TerminalArgs term;
    RecordingSink s;
    bytes p = packed_erc20_deposit(custom, addr(0x77), 9, {});
    CHECK(drive(eng, input(cfg.erc20_portal, p, 0), s, term) == Completion::Accept);
    CHECK(s.last_report().find("\"accounts_drive\":true") != std::string::npos);
    bytes_view drive_bytes = eng.ledger().accounts_drive();
    CHECK(drive_bytes[0] == 9 && drive_bytes[12 + 19] == 0x77);
    unlink(path.c_str());
}

// The machine builds region B with libcma's buffer mode plus a footer written by ledger.cpp; the host
// reopens it with libcma's file mode. Same ops on both paths must give the same bytes, and the
// machine-built region must reopen on the host.
void test_region_b_nvram_layout_matches_file_mode() {
    Config cfg = test_config();
    std::string file_path = temp_state_file();
    std::string nvram_path = temp_state_file();
    auto ops = [&](Ledger &l) {
        Address u = addr(0x11);
        CHECK(l.deposit(AccountRef::of_wallet(u), AssetRef::ether(), U256::from_u64(5)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u), AssetRef::erc20(addr(0x99)), U256::from_u64(77)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(addr(0x22)), AssetRef::erc721(addr(0x33), U256::from_u64(9)), U256::from_u64(1)) == 0);
        CHECK(l.withdraw(AccountRef::of_wallet(u), AssetRef::ether(), U256::from_u64(2)) == 0);
        CHECK(l.deposit(AccountRef::of_wallet(u), AssetRef::erc20(cfg.token), U256::from_u64(1000)) == 0);
        l.flush();
    };
    {
        Ledger as_file(cfg, file_path, false);
        ops(as_file);
    }
    {
        Ledger as_nvram(cfg, nvram_path, true);
        ops(as_nvram);
    }
    bytes a = read_file(file_path);
    bytes b = read_file(nvram_path);
    CHECK(a.size() == cfg.state_length() && b.size() == a.size());
    CHECK(std::equal(a.begin() + cfg.region_a_length, a.begin() + cfg.region_c_offset(), b.begin() + cfg.region_a_length));
    CHECK(a == b);
    {
        Ledger reopened(cfg, nvram_path, false); // the host path over machine-built bytes
        U256 bal;
        CHECK(reopened.balance(AccountRef::of_wallet(addr(0x11)), AssetRef::ether(), bal) == 0 && bal.to_u64() == 3);
        CHECK(reopened.balance(AccountRef::of_wallet(addr(0x11)), AssetRef::erc20(addr(0x99)), bal) == 0 && bal.to_u64() == 77);
        CHECK(reopened.balance(AccountRef::of_wallet(addr(0x22)), AssetRef::erc721(addr(0x33), U256::from_u64(9)), bal) == 0 && bal.to_u64() == 1);
        CHECK(reopened.deposit(AccountRef::of_wallet(addr(0x44)), AssetRef::ether(), U256::from_u64(1)) == 0);
    }
    bool refused = false;
    try {
        Ledger again(cfg, nvram_path, true); // a live range is never reopened in buffer mode
    } catch (const std::runtime_error &) {
        refused = true;
    }
    CHECK(refused);
    unlink(file_path.c_str());
    unlink(nvram_path.c_str());
}

void test_meta_and_nonces() {
    std::vector<uint8_t> region(64u << 10, 0);
    MetaView meta{region.data()};
    CHECK(!meta.has_magic());
    meta.initialize(2, addr(0x5e), addr(0x70));
    CHECK(meta.has_magic() && meta.deposit_policy() == 2 && meta.sequencer() == addr(0x5e) && meta.token() == addr(0x70));
    CHECK(meta.executed_input_count() == 0 && meta.last_executed_safe_block() == 0 && meta.nonce_count() == 0);
    meta.set_progress(5, 700);
    CHECK(meta.executed_input_count() == 5 && meta.last_executed_safe_block() == 700);
    // fixed offsets (docs/sequencer.md)
    CHECK(std::memcmp(region.data(), "QATESTR1", 8) == 0 && region[8] == 1 && region[16] == 5 && region[24] == 0xbc);
    CHECK(region[25] == 0x02 && region[40] == 2 && region[48 + 19] == 0x5e && region[68 + 19] == 0x70);

    NonceTable t(region.data() + META_PAGE_LENGTH, region.size() - META_PAGE_LENGTH, meta);
    CHECK(t.slots() == (region.size() - META_PAGE_LENGTH) / 32);
    Address a = addr(0x01);
    Address b = addr(0x02);
    CHECK(t.get(a) == 0 && t.can_insert(a));
    CHECK(t.set(a, 1) && t.get(a) == 1 && meta.nonce_count() == 1);
    CHECK(t.set(a, 2) && t.get(a) == 2 && meta.nonce_count() == 1);
    CHECK(t.set(b, 7) && t.get(b) == 7 && t.get(a) == 2 && meta.nonce_count() == 2);
    // the zero address is never a sender
    CHECK(!t.can_insert(Address{}) && !t.set(Address{}, 1) && t.get(Address{}) == 0);
    // probe chains: addresses with the same low 32 bits land in consecutive slots
    Address c1 = addr(0x03);
    Address c2 = addr(0x03);
    c2.b[0] = 0xaa;
    CHECK(t.set(c1, 10) && t.set(c2, 20) && t.get(c1) == 10 && t.get(c2) == 20);
    // 50 % load limit: fill until refused, existing senders still update
    size_t inserted = meta.nonce_count();
    for (uint32_t i = 0; inserted < t.slots() / 2; ++i) {
        Address x;
        x.b[16] = static_cast<uint8_t>(i >> 8);
        x.b[17] = static_cast<uint8_t>(i & 0xff);
        x.b[0] = 0x77;
        if (t.get(x) == 0 && t.set(x, 1)) {
            ++inserted;
        }
    }
    CHECK(meta.nonce_count() == t.slots() / 2);
    Address extra = addr(0x09);
    extra.b[1] = 0x99;
    CHECK(!t.can_insert(extra) && !t.set(extra, 1));
    CHECK(t.set(a, 3) && t.get(a) == 3);
}

void test_fee_to_linear() {
    // reference values from an exact big-integer evaluation of sequencer-core/src/fee.rs (seq/guest
    // compares every exponent against the Rust crate)
    CHECK(fee::max_exponent() == 17101);
    U256 v;
    CHECK(fee::to_linear(0, v) && v.to_u64() == 1);
    CHECK(fee::to_linear(1, v) && v.to_u64() == 1);
    CHECK(fee::to_linear(100, v) && v.to_u64() == 2);
    CHECK(fee::to_linear(1356, v) && v.to_u64() == 38276);
    CHECK(fee::to_linear(17101, v) && to_hex(v) == "0x0000000000000000ff853cbad77d81420c3129ed78d03683ecdfc2c89dd67c94");
    CHECK(!fee::to_linear(17102, v) && !fee::to_linear(0xffff, v));
}

void test_user_ops() {
    Config cfg = test_config();
    std::string path = temp_state_file();
    Address seq = addr(0x5e);
    Engine::genesis(cfg, path, seq);
    Engine eng(cfg, path);
    Address user = addr(0x77);
    TerminalArgs term;
    UserOpInvalid inv;
    // no balance: a fee of exponent 0 (1 unit) cannot be covered
    CHECK(!eng.validate_user_op(user, 0, 0, inv) && inv.reason == UserOpInvalid::Reason::FeeBalance);
    CHECK(inv.required.to_u64() == 1 && inv.available.is_zero());
    // wrong nonce is reported first
    CHECK(!eng.validate_user_op(user, 1, 0, inv) && inv.reason == UserOpInvalid::Reason::Nonce && inv.expected == 0 && inv.got == 1);
    {
        RecordingSink s;
        bytes p = packed_erc20_deposit(cfg.token, user, 1000, {});
        CHECK(drive(eng, input(cfg.erc20_portal, p, 0), s, term) == Completion::Accept);
    }
    // fee exponent 1356 -> 38276 units: more than the 1000 deposited
    CHECK(!eng.validate_user_op(user, 0, 1356, inv) && inv.reason == UserOpInvalid::Reason::FeeBalance);
    CHECK(inv.required.to_u64() == 38276 && inv.available.to_u64() == 1000);
    // fee 100 -> 2 units: accepted; execution charges the submitter and advances the nonce
    CHECK(eng.validate_user_op(user, 0, 100, inv));
    {
        RecordingSink s;
        bytes p = call(Cmd::EmitNotices, [&](abi::Writer &w) {
            w.uint(2);
            w.uint(8);
            w.uint(0);
        });
        DirectInput in = input(user, p, 1);
        in.block_number = 500; // the frame's safe block
        CHECK(eng.execute_user_op(in, 100, s, term) == Completion::Accept);
        eng.commit_progress(in.block_number);
        CHECK(s.count("notice") == 2);
        U256 bal;
        CHECK(eng.ledger().balance(AccountRef::of_wallet(user), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 998);
        CHECK(eng.ledger().balance(AccountRef::of_wallet(seq), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 2);
        CHECK(eng.nonce_of(user) == 1 && eng.progress().executed_input_count == 2 && eng.progress().last_block == 500);
    }
    // a business failure still consumes the nonce and the fee
    CHECK(!eng.validate_user_op(user, 0, 0, inv) && inv.expected == 1 && inv.got == 0);
    CHECK(eng.validate_user_op(user, 1, 0, inv));
    {
        RecordingSink s;
        bytes p = {0xde, 0xad, 0xbe, 0xef, 0x00};
        DirectInput in = input(user, p, 2);
        CHECK(eng.execute_user_op(in, 0, s, term) == Completion::Reject);
        CHECK(s.last_report().find("unknown input") != std::string::npos);
        U256 bal;
        CHECK(eng.ledger().balance(AccountRef::of_wallet(user), AssetRef::erc20(cfg.token), bal) == 0 && bal.to_u64() == 997);
        CHECK(eng.nonce_of(user) == 2);
    }
    // inspect: ledger_getNonce and qa_fee
    {
        RecordingSink s;
        std::string q = "{\"method\":\"ledger_getNonce\",\"params\":[\"" + to_hex(user) + "\"]}";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(q.data()), q.size()), s, term) == Completion::Accept);
        CHECK(s.items.back().payload.size() == 32 && s.items.back().payload[31] == 2);
        RecordingSink s2;
        std::string f = "{\"method\":\"qa_fee\",\"params\":[1356]}";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(f.data()), f.size()), s2, term) == Completion::Accept);
        CHECK(s2.items.back().payload[30] == 0x95 && s2.items.back().payload[31] == 0x84);
        RecordingSink s3;
        const char *st = "state";
        CHECK(eng.inspect(bytes_view(reinterpret_cast<const uint8_t *>(st), 5), s3, term) == Completion::Accept);
        CHECK(s3.last_report().find("\"nonce_count\":1") != std::string::npos);
        CHECK(s3.last_report().find("\"sequencer_address\":\"" + to_hex(seq) + "\"") != std::string::npos);
    }
    // nonces survive a reopen
    {
        Engine again(cfg, path);
        CHECK(again.nonce_of(user) == 2 && again.nonce_of(seq) == 0);
    }
    unlink(path.c_str());
}

// ------------------------------------------------------------------ the sequencer's C seam
struct AuxCapture {
    std::vector<std::string> reports;
    std::vector<bytes> delegates;
    std::vector<bytes> raws;
};

int aux_report(void *ctx, const uint8_t *d, uint64_t n) {
    static_cast<AuxCapture *>(ctx)->reports.emplace_back(reinterpret_cast<const char *>(d), n);
    return 0;
}
int aux_raw(void *ctx, const uint8_t *d, uint64_t n) {
    static_cast<AuxCapture *>(ctx)->raws.emplace_back(d, d + n);
    return 0;
}
int aux_delegate(void *ctx, const uint8_t *, const uint8_t *d, uint64_t n) {
    static_cast<AuxCapture *>(ctx)->delegates.emplace_back(d, d + n);
    return 0;
}

bytes read_file(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    bytes out;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return out;
}

ApplicationEngineEthereumAddress abi_addr(const Address &a) {
    ApplicationEngineEthereumAddress r{};
    std::copy(a.b.begin(), a.b.end(), std::begin(r.bytes));
    return r;
}

ApplicationEngineDirectInput abi_direct(const Address &sender, const bytes &payload, uint64_t block) {
    ApplicationEngineDirectInput in{};
    in.sender = abi_addr(sender);
    in.block_number = block;
    in.payload = ApplicationEngineByteSpan{payload.data(), payload.size()};
    return in;
}

void test_app_engine_abi() {
    Config cfg = test_config();
    setenv("QA_LOG", "0", 1);
    setenv("QA_SINGLE_ACCOUNTS", "1024", 1);
    setenv("QA_REGION_A_LENGTH", std::to_string(cfg.region_a_length).c_str(), 1);
    setenv("QA_REGION_B_LENGTH", std::to_string(cfg.region_b_length).c_str(), 1);
    setenv("QA_REGION_C_LENGTH", std::to_string(cfg.region_c_length).c_str(), 1);
    setenv("QA_MULTI_ACCOUNTS", "256", 1);
    setenv("QA_MULTI_ASSETS", "16", 1);
    setenv("QA_MULTI_BALANCES", "1024", 1);
    setenv("QA_SAFE_ERC20_TRANSFER", to_hex(addr(0x55)).c_str(), 1);
    char dirt[] = "/tmp/qa-tester-abi-XXXXXX";
    CHECK(mkdtemp(dirt) != nullptr);
    std::string dir = dirt;
    setenv("QA_ENGINE_WORKDIR", dir.c_str(), 1);
    std::string genesis = dir + "/genesis";
    Address seq = addr(0x5e);
    Address user = addr(0x77);

    CHECK(application_engine_max_method_payload_bytes() == 2048);
    CHECK(std::string(application_engine_state_file_in_dump("/x/y")) == "/x/y");
    CHECK(qatester_genesis(genesis.c_str(), seq.b.data(), nullptr) == 0);
    CHECK(qatester_genesis(genesis.c_str(), seq.b.data(), nullptr) != 0); // refuses an initialised state
    struct stat st{};
    CHECK(stat(genesis.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) == cfg.state_length());
    bytes source = read_file(genesis);

    ApplicationEngine *eng = nullptr;
    CHECK(application_engine_from_dump((dir + "/missing").c_str(), &eng) == APPLICATION_ENGINE_STATUS_NOT_FOUND);
    {
        std::string shortf = dir + "/short";
        FILE *f = std::fopen(shortf.c_str(), "wb");
        std::fputs("xx", f);
        std::fclose(f);
        CHECK(application_engine_from_dump(shortf.c_str(), &eng) == APPLICATION_ENGINE_STATUS_INVALID_DUMP);
        std::string blank = dir + "/blank";
        f = std::fopen(blank.c_str(), "wb");
        std::fclose(f);
        CHECK(truncate(blank.c_str(), static_cast<off_t>(cfg.state_length())) == 0);
        CHECK(application_engine_from_dump(blank.c_str(), &eng) == APPLICATION_ENGINE_STATUS_INVALID_DUMP);
        CHECK(std::string(application_engine_get_last_error_message()).find("magic") != std::string::npos);
    }
    CHECK(application_engine_from_dump(genesis.c_str(), &eng) == APPLICATION_ENGINE_STATUS_OK && eng != nullptr);
    AuxCapture aux;
    qatester_aux_sink sink{&aux, aux_report, aux_raw, aux_delegate, nullptr};
    CHECK(qatester_live_attach_aux(&sink) == -ENOENT); // no machine ranges: no live engine
    qatester_set_live_engine(eng);
    CHECK(qatester_live_attach_aux(&sink) == 0);
    {
        qatester_input_context ictx{};
        ictx.index = 42;
        CHECK(qatester_live_set_input_context(&ictx) == 0);
        CHECK(qatester_live_set_scheduler_status(3, 1) == 0);
        std::string got;
        auto cb = [](void *c, const uint8_t *d, uint64_t n) {
            static_cast<std::string *>(c)->assign(reinterpret_cast<const char *>(d), n);
            return 0;
        };
        const char *st = "state";
        CHECK(qatester_live_inspect(reinterpret_cast<const uint8_t *>(st), 5, cb, &got) == 0);
        CHECK(got.find("\"scheduler\":{\"next_batch_nonce\":3,\"queued_directs\":1}") != std::string::npos);
        std::string ex = "{\"method\":\"qa_exception\"}";
        CHECK(qatester_live_inspect(reinterpret_cast<const uint8_t *>(ex.data()), ex.size(), cb, &got) == 0);
        CHECK(got.find("terminal inspect method is a no-op") != std::string::npos);
    }
    ApplicationEngineProgress prog{};
    application_engine_progress(eng, &prog);
    CHECK(prog.executed_input_count == 0 && prog.last_executed_safe_block == 0);

    uint64_t n = 99;
    // 1. deposit through the portal: no outputs, a report, progress 1
    bytes dep = packed_erc20_deposit(cfg.token, user, 1000000, {});
    ApplicationEngineDirectInput in = abi_direct(cfg.erc20_portal, dep, 10);
    CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
    CHECK(aux.reports.size() == 1 && aux.reports[0].find("\"credited\":true") != std::string::npos);
    application_engine_progress(eng, &prog);
    CHECK(prog.executed_input_count == 1 && prog.last_executed_safe_block == 10);
    CHECK(read_file(genesis) == source); // the source dump is never written

    // 2. notices queue until drained; executing while queued is refused; one drain too many too
    bytes two = call(Cmd::EmitNotices, [&](abi::Writer &w) {
        w.uint(2);
        w.uint(8);
        w.uint(2);
    });
    in = abi_direct(user, two, 11);
    CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 2);
    CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_INTERNAL_ERROR);
    ApplicationEngineOutput out{};
    CHECK(application_engine_drain_output(eng, &out) == APPLICATION_ENGINE_STATUS_OK && out.kind == APPLICATION_ENGINE_OUTPUT_NOTICE);
    CHECK(out.values.notice.size == 8 && out.values.notice.data[0] == 0xff);
    const uint8_t *first = out.values.notice.data;
    CHECK(application_engine_drain_output(eng, &out) == APPLICATION_ENGINE_STATUS_OK && out.values.notice.data != first);
    CHECK(application_engine_drain_output(eng, &out) == APPLICATION_ENGINE_STATUS_INTERNAL_ERROR);

    // 3. a withdrawal is a voucher with its full value
    {
        abi::Writer w;
        w.selector(0x4f94d342); // WithdrawErc20(address,uint256,bytes)
        w.address(cfg.token);
        w.word(U256::from_u64(400000));
        w.dyn_bytes(bytes{});
        bytes p = w.finish();
        in = abi_direct(user, p, 12);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 1);
        CHECK(application_engine_drain_output(eng, &out) == APPLICATION_ENGINE_STATUS_OK && out.kind == APPLICATION_ENGINE_OUTPUT_VOUCHER);
        CHECK(std::equal(cfg.token.b.begin(), cfg.token.b.end(), std::begin(out.values.voucher.destination.bytes)));
        CHECK(out.values.voucher.payload.size == 68 && out.values.voucher.payload.data[0] == 0xa9);
    }
    // 4. machine-only outputs go to the aux sink and never queue
    {
        bytes p = call(Cmd::WithdrawErc20Safe, [&](abi::Writer &w) {
            w.address(Address{});
            w.address(cfg.token);
            w.word(U256::from_u64(100000));
            w.dyn_bytes(bytes{});
        });
        in = abi_direct(user, p, 13);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
        CHECK(aux.delegates.size() == 1 && aux.delegates[0].size() == 100);
        bytes r = call(Cmd::EmitRawOutput, [&](abi::Writer &w) { w.dyn_bytes(bytes{1, 2, 3}); });
        in = abi_direct(user, r, 13);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
        CHECK(aux.raws.size() == 1 && aux.raws[0] == bytes({1, 2, 3}));
    }
    // 5. terminal commands are counted no-ops with a report; rejected inputs count too
    {
        size_t before = aux.reports.size();
        bytes h = call(Cmd::Halt, [&](abi::Writer &w) { w.uint(2); });
        in = abi_direct(user, h, 20);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
        CHECK(aux.reports.size() == before + 1 && aux.reports.back().find("terminal command halt is a no-op") != std::string::npos);
        bytes unk = {0xde, 0xad, 0xbe, 0xef, 0x00};
        in = abi_direct(user, unk, 21);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
        application_engine_progress(eng, &prog);
        CHECK(prog.executed_input_count == 7 && prog.last_executed_safe_block == 21);
    }
    // 6. a batch is not atomic: the withdrawal before the failing call stays, the voucher is dropped
    {
        bytes a = call(Cmd::WithdrawErc20Safe, [&](abi::Writer &w) {
            w.address(Address{});
            w.address(cfg.token);
            w.word(U256::from_u64(1));
            w.dyn_bytes(bytes{});
        });
        bytes v = call(Cmd::EmitVouchers, [&](abi::Writer &w) {
            w.uint(1);
            w.address(addr(0x42));
            w.word(U256{});
            w.dyn_bytes(bytes{});
        });
        bytes bad = {0xde, 0xad, 0xbe, 0xef, 0x00};
        bytes arr;
        auto wordp = [&](uint64_t x) {
            U256 u = U256::from_u64(x);
            arr.insert(arr.end(), u.b.begin(), u.b.end());
        };
        std::vector<bytes> items = {a, v, bad};
        wordp(items.size());
        size_t off = 32 * items.size();
        std::vector<bytes> padded;
        for (auto &it : items) {
            wordp(off);
            bytes pd;
            U256 l = U256::from_u64(it.size());
            pd.insert(pd.end(), l.b.begin(), l.b.end());
            pd.insert(pd.end(), it.begin(), it.end());
            pd.insert(pd.end(), (32 - it.size() % 32) % 32, 0);
            off += pd.size();
            padded.push_back(pd);
        }
        for (auto &pd : padded) {
            arr.insert(arr.end(), pd.begin(), pd.end());
        }
        abi::Writer w;
        w.selector(selector_of(Cmd::Batch));
        w.uint(32);
        bytes head = w.finish();
        head.insert(head.end(), arr.begin(), arr.end());
        in = abi_direct(user, head, 22);
        CHECK(application_engine_execute_direct_input(eng, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 0);
        CHECK(aux.delegates.size() == 2); // the safe withdrawal inside the batch was emitted
    }
    // 7. user ops through the seam
    {
        ApplicationEngineInvalid inv{};
        ApplicationEngineUserOp op{};
        op.nonce = 1;
        op.max_fee = 100;
        ApplicationEngineEthereumAddress s = abi_addr(user);
        CHECK(application_engine_validate_user_op(eng, &s, &op, 100, &inv) == APPLICATION_ENGINE_STATUS_INVALID);
        CHECK(inv.reason == APPLICATION_ENGINE_INVALID_NONCE && inv.values.nonce.expected == 0 && inv.values.nonce.got == 1);
        op.nonce = 0;
        CHECK(application_engine_validate_user_op(eng, &s, &op, 17101, &inv) == APPLICATION_ENGINE_STATUS_INVALID);
        CHECK(inv.reason == APPLICATION_ENGINE_INSUFFICIENT_FEE_BALANCE && inv.values.fee_balance.required.bytes[31] == 0x94);
        CHECK(application_engine_validate_user_op(eng, &s, &op, 100, &inv) == APPLICATION_ENGINE_STATUS_OK);
        bytes note = call(Cmd::EmitNotice, [&](abi::Writer &w) { w.dyn_bytes(bytes{7}); });
        ApplicationEngineValidUserOp valid{};
        valid.sender = s;
        valid.fee = 100;
        valid.data = ApplicationEngineByteSpan{note.data(), note.size()};
        CHECK(application_engine_execute_valid_user_op(eng, &valid, 500, &n) == APPLICATION_ENGINE_STATUS_OK && n == 1);
        CHECK(application_engine_drain_output(eng, &out) == APPLICATION_ENGINE_STATUS_OK && out.kind == APPLICATION_ENGINE_OUTPUT_NOTICE);
        application_engine_progress(eng, &prog);
        CHECK(prog.executed_input_count == 9 && prog.last_executed_safe_block == 500);
        op.nonce = 1;
        CHECK(application_engine_validate_user_op(eng, &s, &op, 0, &inv) == APPLICATION_ENGINE_STATUS_OK);
    }
    // 8. dumps: crash-durable copy, immutable, independently restorable; the working copy goes away
    std::string dump = dir + "/dump1";
    CHECK(application_engine_create_dump(eng, dump.c_str()) == APPLICATION_ENGINE_STATUS_OK);
    CHECK(application_engine_create_dump(eng, dump.c_str()) == APPLICATION_ENGINE_STATUS_IO_ERROR); // exists
    CHECK(application_engine_create_dump(eng, (dir + "/no/such/dir").c_str()) == APPLICATION_ENGINE_STATUS_NOT_FOUND);
    bytes dumped = read_file(dump);
    CHECK(dumped.size() == cfg.state_length() && dumped != source);
    {
        ApplicationEngine *other = nullptr;
        CHECK(application_engine_from_dump(dump.c_str(), &other) == APPLICATION_ENGINE_STATUS_OK);
        ApplicationEngineProgress p2{};
        application_engine_progress(other, &p2);
        CHECK(p2.executed_input_count == 9 && p2.last_executed_safe_block == 500);
        ApplicationEngineInvalid inv{};
        ApplicationEngineUserOp op{};
        op.nonce = 1;
        ApplicationEngineEthereumAddress s = abi_addr(user);
        CHECK(application_engine_validate_user_op(other, &s, &op, 0, &inv) == APPLICATION_ENGINE_STATUS_OK);
        // the two instances evolve independently
        bytes h = call(Cmd::EmitNotice, [&](abi::Writer &w) { w.dyn_bytes(bytes{1}); });
        in = abi_direct(user, h, 600);
        CHECK(application_engine_execute_direct_input(other, &in, &n) == APPLICATION_ENGINE_STATUS_OK && n == 1);
        CHECK(application_engine_drain_output(other, &out) == APPLICATION_ENGINE_STATUS_OK);
        application_engine_progress(eng, &prog);
        CHECK(prog.executed_input_count == 9);
        application_engine_destroy(other);
    }
    CHECK(read_file(dump) == dumped);
    application_engine_destroy(eng);
    CHECK(qatester_live_attach_aux(nullptr) == -ENOENT); // destroying the live engine clears it
    // only the genesis, the dumps and the helper files remain: no working copies
    {
        int rc = std::system(("ls " + dir + "/qa-tester-engine-* >/dev/null 2>&1").c_str());
        CHECK(rc != 0);
    }
    CHECK(std::system(("rm -rf " + dir).c_str()) == 0);
}

} // namespace

int main() {
    test_selectors_match_cast();
    test_abi_roundtrip();
    test_u256_text();
    test_ledger_routing_and_records();
    test_engine_flow();
    test_region_b_nvram_layout_matches_file_mode();
    test_genesis_pins_a_configured_token();
    test_meta_and_nonces();
    test_fee_to_linear();
    test_user_ops();
    test_app_engine_abi();
    std::printf("unit tests passed (%d checks)\n", g_checks);
    return 0;
}
