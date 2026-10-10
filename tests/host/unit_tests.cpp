// Host unit tests for libqatester (no framework; each check aborts with a message on failure).
#include "qatester/abi.h"
#include "qatester/commands.h"
#include "qatester/config.h"
#include "qatester/engine.h"
#include "qatester/ledger.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

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
    Engine eng(cfg, path);
    Address user = addr(0x77);
    TerminalArgs term;

    // 1. ERC-20 deposit of the configured token via the portal
    {
        RecordingSink s;
        bytes exec = {0xde, 0xad};
        bytes p = packed_erc20_deposit(cfg.token, user, 1000000, exec);
        CHECK(eng.execute_direct_input(input(cfg.erc20_portal, p, 0), s, term) == Completion::Accept);
        CHECK(s.count("report") == 1);
        CHECK(s.last_report().find("\"credited\":true") != std::string::npos);
        CHECK(s.last_report().find("\"exec_layer_data\":\"0xdead\"") != std::string::npos);
        CHECK(s.last_report().find("\"accounts_drive\":true") != std::string::npos);
    }
    // 2. a deposit of another token is credited too, but not on the accounts drive
    {
        RecordingSink s;
        bytes p = packed_erc20_deposit(addr(0x99), user, 50, {});
        CHECK(eng.execute_direct_input(input(cfg.erc20_portal, p, 1), s, term) == Completion::Accept);
        CHECK(s.last_report().find("\"accounts_drive\":false") != std::string::npos);
    }
    // 3. malformed deposit payload (too short) -> reject with a report
    {
        RecordingSink s;
        bytes p = {1, 2, 3};
        CHECK(eng.execute_direct_input(input(cfg.erc20_portal, p, 2), s, term) == Completion::Reject);
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
        CHECK(eng.execute_direct_input(input(user, p, 3), s, term) == Completion::Accept);
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
        CHECK(eng.execute_direct_input(input(user, p, 4), s, term) == Completion::Accept);
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
        CHECK(eng.execute_direct_input(input(user, p, 5), s, term) == Completion::Reject);
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
        CHECK(eng.execute_direct_input(input(user, p, 6), s, term) == Completion::Accept);
        CHECK(s.count("notice") == 3 && s.items[0].payload.size() == 300 && s.items[0].payload[255] == 0xff && s.items[0].payload[256] == 0);
        RecordingSink f;
        f.fail_after = 1;
        CHECK(eng.execute_direct_input(input(user, p, 7), f, term) == Completion::Reject);
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
        CHECK(eng.execute_direct_input(input(user, head, 8), s, term) == Completion::CorruptRootValue);
        CHECK(s.count("report") == 1 && s.count("voucher") == 1 && s.count("notice") == 0);
    }
    // 10. terminal commands carry their arguments
    {
        RecordingSink s;
        bytes p = call(Cmd::ManualYield, [&](abi::Writer &w) {
            w.uint(8);
            w.uint(5);
        });
        CHECK(eng.execute_direct_input(input(user, p, 9), s, term) == Completion::UnexpectedYield);
        CHECK(term.yield_reason == 8 && term.yield_data == 5);
        bytes h = call(Cmd::Halt, [&](abi::Writer &w) { w.uint(2); });
        CHECK(eng.execute_direct_input(input(user, h, 10), s, term) == Completion::Halt && term.mode == 2);
        bytes e = call(Cmd::RaiseException, [&](abi::Writer &w) { w.dyn_bytes(bytes{'x', 'y'}); });
        CHECK(eng.execute_direct_input(input(user, e, 11), s, term) == Completion::Exception && term.payload == bytes({'x', 'y'}));
        bytes u = {0xde, 0xad, 0xbe, 0xef, 0x00};
        CHECK(eng.execute_direct_input(input(user, u, 12), s, term) == Completion::Reject);
        CHECK(s.last_report().find("unknown input") != std::string::npos);
        CHECK(eng.execute_direct_input(input(user, bytes{}, 13), s, term) == Completion::Reject);
        CHECK(s.last_report().find("empty payload") != std::string::npos);
    }
    // 11. deposit policy switch
    {
        RecordingSink s;
        bytes p = call(Cmd::SetDepositPolicy, [&](abi::Writer &w) { w.uint(1); });
        CHECK(eng.execute_direct_input(input(user, p, 14), s, term) == Completion::Accept);
        bytes dep = packed_erc20_deposit(addr(0x99), user, 1, {});
        CHECK(eng.execute_direct_input(input(cfg.erc20_portal, dep, 15), s, term) == Completion::Reject);
        CHECK(s.last_report().find("deposit policy") != std::string::npos);
        bytes dep2 = packed_erc20_deposit(cfg.token, user, 1, {});
        CHECK(eng.execute_direct_input(input(cfg.erc20_portal, dep2, 16), s, term) == Completion::Accept);
    }
    // 12. progress counters: accepted inputs only
    CHECK(eng.progress().executed_input_count == 7);
    CHECK(eng.progress().last_block == 116);
    unlink(path.c_str());
}

} // namespace

int main() {
    test_selectors_match_cast();
    test_abi_roundtrip();
    test_u256_text();
    test_ledger_routing_and_records();
    test_engine_flow();
    std::printf("unit tests passed (%d checks)\n", g_checks);
    return 0;
}
