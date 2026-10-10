// Engine: turns a direct input (or inspect query) into ledger changes, emissions and a completion.
#include "qatester/engine.h"

#include "qatester/abi.h"
#include "qatester/commands.h"

extern "C" {
#include <libcma/parser.h>
#include <libcmt/abi.h>
#include <libcmt/keccak.h>
#include <libcmt/rollup.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace qatester {

using json = nlohmann::json;

namespace {

constexpr size_t MAX_INSPECT_JSON = 1u << 20; // JSON inspects larger than this are treated as raw echo

[[nodiscard]] std::string dec(const U256 &v) {
    return u256_to_dec(v);
}

[[nodiscard]] U256 from_cma(const cmt_abi_u256_t &a) {
    U256 v;
    std::copy(std::begin(a.data), std::end(a.data), v.b.begin());
    return v;
}

[[nodiscard]] Address from_cma(const cmt_abi_address_t &a) {
    Address v;
    std::copy(std::begin(a.data), std::end(a.data), v.b.begin());
    return v;
}

[[nodiscard]] bytes_view view(const cmt_abi_bytes_t &b) {
    return bytes_view(static_cast<const uint8_t *>(b.data), b.data == nullptr ? 0 : b.length);
}

[[nodiscard]] std::string keccak_hex(bytes_view data) {
    uint8_t md[CMT_KECCAK_LENGTH];
    cmt_keccak_t st;
    cmt_keccak_init(&st);
    cmt_keccak_update(&st, data.size(), data.data());
    cmt_keccak_final(&st, md);
    return to_hex(bytes_view(md, sizeof md));
}

} // namespace

void fill_pattern(bytes &buf, size_t size, uint8_t pattern, uint64_t seed) {
    buf.resize(size);
    switch (pattern) {
        case 1:
            std::fill(buf.begin(), buf.end(), uint8_t{0});
            break;
        case 2:
            std::fill(buf.begin(), buf.end(), uint8_t{0xff});
            break;
        case 3: {
            uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            for (size_t i = 0; i < size; ++i) {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                buf[i] = static_cast<uint8_t>(x >> 56);
            }
            break;
        }
        default:
            for (size_t i = 0; i < size; ++i) {
                buf[i] = static_cast<uint8_t>(i & 0xffU);
            }
            break;
    }
}

uint64_t burn_cycles(uint64_t iterations) {
    volatile uint64_t acc = 0x9e3779b97f4a7c15ULL;
    for (uint64_t i = 0; i < iterations; ++i) {
        uint64_t x = acc;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc = x + i;
    }
    return acc;
}

struct Engine::Impl {
    Config cfg;
    Ledger ledger;
    Progress prog;
    uint64_t rejected_count{0};
    uint64_t current_index{0};
    bytes scratch;

    Impl(Config c, const std::string &state_file, const std::string &ledger_file) : cfg(c), ledger(c, state_file, ledger_file) {}

    __attribute__((format(printf, 2, 3))) void log(const char *fmt, ...) {
        if (!cfg.log) {
            return;
        }
        std::va_list ap;
        va_start(ap, fmt);
        std::fprintf(stderr, "[qa-tester] ");
        std::vfprintf(stderr, fmt, ap);
        std::fprintf(stderr, "\n");
        va_end(ap);
    }

    // --- reporting helpers -------------------------------------------------------------------
    int report_json(OutputSink &sink, const json &j) {
        std::string s = j.dump();
        return sink.report(bytes_view(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
    }

    Completion fail(OutputSink &sink, const std::string &what, int code = 0) {
        log("reject: %s (%d)", what.c_str(), code);
        json j{{"error", what}};
        if (code != 0) {
            j["code"] = code;
        }
        (void) report_json(sink, j);
        return Completion::Reject;
    }

    // --- portal deposits ---------------------------------------------------------------------
    [[nodiscard]] cma_parser_input_type_t portal_kind(const Address &sender) const {
        if (sender == cfg.ether_portal) {
            return CMA_PARSER_INPUT_TYPE_ETHER_DEPOSIT;
        }
        if (sender == cfg.erc20_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC20_DEPOSIT;
        }
        if (sender == cfg.erc721_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC721_DEPOSIT;
        }
        if (sender == cfg.erc1155_single_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_DEPOSIT;
        }
        if (sender == cfg.erc1155_batch_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_DEPOSIT;
        }
        return CMA_PARSER_INPUT_TYPE_NONE;
    }

    static cmt_rollup_advance_t to_cmt(const DirectInput &in) {
        cmt_rollup_advance_t a{};
        a.chain_id = in.chain_id;
        std::copy(in.app_contract.b.begin(), in.app_contract.b.end(), std::begin(a.app_contract.data));
        std::copy(in.sender.b.begin(), in.sender.b.end(), std::begin(a.msg_sender.data));
        a.block_number = in.block_number;
        a.block_timestamp = in.block_timestamp;
        a.index = in.index;
        a.payload.length = in.payload.size();
        a.payload.data = const_cast<uint8_t *>(in.payload.data());
        return a;
    }

    Completion handle_deposit(const DirectInput &in, cma_parser_input_type_t kind, OutputSink &sink) {
        cmt_rollup_advance_t adv = to_cmt(in);
        cma_parser_input_t p{};
        int rc = cma_parser_decode_advance(kind, &adv, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("unable to decode deposit: ") + cma_parser_get_last_error_message(), rc);
        }
        json rep;
        Address depositor;
        AssetRef asset;
        std::vector<std::pair<AssetRef, U256>> credits;
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_ETHER_DEPOSIT:
                depositor = from_cma(p.ether_deposit.sender);
                asset = AssetRef::ether();
                credits.emplace_back(asset, from_cma(p.ether_deposit.amount));
                rep = {{"deposit", "ether"}, {"sender", to_hex(depositor)}, {"amount", dec(from_cma(p.ether_deposit.amount))},
                    {"exec_layer_data", to_hex(view(p.ether_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC20_DEPOSIT:
                depositor = from_cma(p.erc20_deposit.sender);
                asset = AssetRef::erc20(from_cma(p.erc20_deposit.token));
                credits.emplace_back(asset, from_cma(p.erc20_deposit.amount));
                rep = {{"deposit", "erc20"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"amount", dec(from_cma(p.erc20_deposit.amount))},
                    {"exec_layer_data", to_hex(view(p.erc20_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC721_DEPOSIT:
                depositor = from_cma(p.erc721_deposit.sender);
                asset = AssetRef::erc721(from_cma(p.erc721_deposit.token), from_cma(p.erc721_deposit.token_id));
                credits.emplace_back(asset, U256::from_u64(1));
                rep = {{"deposit", "erc721"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"token_id", dec(asset.id)}, {"base_layer_data", to_hex(view(p.erc721_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc721_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_DEPOSIT:
                depositor = from_cma(p.erc1155_single_deposit.sender);
                asset = AssetRef::erc1155(from_cma(p.erc1155_single_deposit.token),
                    from_cma(p.erc1155_single_deposit.token_id));
                credits.emplace_back(asset, from_cma(p.erc1155_single_deposit.amount));
                rep = {{"deposit", "erc1155_single"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"token_id", dec(asset.id)}, {"amount", dec(from_cma(p.erc1155_single_deposit.amount))},
                    {"base_layer_data", to_hex(view(p.erc1155_single_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc1155_single_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_DEPOSIT: {
                depositor = from_cma(p.erc1155_batch_deposit.sender);
                Address token = from_cma(p.erc1155_batch_deposit.token);
                json ids = json::array();
                json amounts = json::array();
                if (p.erc1155_batch_deposit.token_ids.length != p.erc1155_batch_deposit.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                for (size_t t = 0; t < p.erc1155_batch_deposit.token_ids.length; ++t) {
                    U256 id;
                    U256 amount;
                    std::copy_n(p.erc1155_batch_deposit.token_ids.data[t], 32, id.b.begin());
                    std::copy_n(p.erc1155_batch_deposit.amounts.data[t], 32, amount.b.begin());
                    credits.emplace_back(AssetRef::erc1155(token, id), amount);
                    ids.push_back(dec(id));
                    amounts.push_back(dec(amount));
                }
                asset = AssetRef::erc1155(token, U256{});
                rep = {{"deposit", "erc1155_batch"}, {"sender", to_hex(depositor)}, {"token", to_hex(token)},
                    {"token_ids", ids}, {"amounts", amounts},
                    {"base_layer_data", to_hex(view(p.erc1155_batch_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc1155_batch_deposit.exec_layer_data))}};
                break;
            }
            default:
                return fail(sink, "unexpected parser type for deposit");
        }
        bool on_drive = ledger.is_accounts_drive_asset(asset);
        rep["accounts_drive"] = on_drive;
        if (cfg.deposit_policy == DepositPolicy::RejectAll ||
            (cfg.deposit_policy == DepositPolicy::RejectNonToken && !on_drive)) {
            rep["credited"] = false;
            rep["error"] = "rejected by deposit policy";
            (void) report_json(sink, rep);
            log("deposit rejected by policy %d", static_cast<int>(cfg.deposit_policy));
            return Completion::Reject;
        }
        AccountRef acc = AccountRef::of_wallet(depositor);
        for (auto &[a, amount] : credits) {
            int lrc = ledger.deposit(acc, a, amount);
            if (lrc != 0) {
                rep["credited"] = false;
                rep["error"] = ledger.last_error();
                rep["code"] = lrc;
                (void) report_json(sink, rep);
                log("deposit failed: %s", ledger.last_error().c_str());
                return Completion::Reject;
            }
        }
        rep["credited"] = true;
        if (int rc2 = report_json(sink, rep); rc2 < 0) {
            return fail(sink, "unable to emit deposit report", rc2);
        }
        return Completion::Accept;
    }

    // --- libcma withdrawals / transfers ------------------------------------------------------
    Completion emit_cma_voucher(OutputSink &sink, cma_parser_voucher_type_t type, const Address &app,
        cma_parser_voucher_data_t &req, size_t payload_size) {
        bytes payload(payload_size);
        cma_voucher_t v{};
        v.payload.length = payload_size;
        v.payload.data = payload_size ? payload.data() : nullptr;
        cma_abi_address_t app_c{};
        std::copy(app.b.begin(), app.b.end(), std::begin(app_c.data));
        int rc = cma_parser_encode_voucher(type, &app_c, &req, &v);
        if (rc < 0) {
            return fail(sink, std::string("unable to encode voucher: ") + cma_parser_get_last_error_message(), rc);
        }
        rc = sink.voucher(from_cma(v.address), from_cma(v.value), view(v.payload));
        if (rc < 0) {
            return fail(sink, "unable to emit voucher", rc);
        }
        return Completion::Accept;
    }

    Completion handle_cma_advance(const DirectInput &in, OutputSink &sink) {
        cmt_rollup_advance_t adv = to_cmt(in);
        cma_parser_input_t p{};
        int rc = cma_parser_decode_advance(CMA_PARSER_INPUT_TYPE_AUTO, &adv, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("unknown input: ") + cma_parser_get_last_error_message(), rc);
        }
        AccountRef me = AccountRef::of_wallet(in.sender);
        cma_parser_voucher_data_t req{};
        std::copy(in.sender.b.begin(), in.sender.b.end(), std::begin(req.receiver.data));
        auto withdraw = [&](const AssetRef &asset, const U256 &amount) -> bool {
            int lrc = ledger.withdraw(me, asset, amount);
            if (lrc != 0) {
                (void) fail(sink, ledger.last_error(), lrc);
                return false;
            }
            return true;
        };
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_ETHER_WITHDRAWAL: {
                U256 amount = from_cma(p.ether_withdrawal.amount);
                if (!withdraw(AssetRef::ether(), amount)) {
                    return Completion::Reject;
                }
                req.ether.amount = p.ether_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ETHER, in.app_contract, req, 0);
            }
            case CMA_PARSER_INPUT_TYPE_ERC20_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc20(from_cma(p.erc20_withdrawal.token));
                if (!withdraw(asset, from_cma(p.erc20_withdrawal.amount))) {
                    return Completion::Reject;
                }
                req.erc20.token = p.erc20_withdrawal.token;
                req.erc20.amount = p.erc20_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC20, in.app_contract, req,
                    CMA_PARSER_ERC20_VOUCHER_PAYLOAD_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC721_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc721(from_cma(p.erc721_withdrawal.token), from_cma(p.erc721_withdrawal.token_id));
                if (!withdraw(asset, U256::from_u64(1))) {
                    return Completion::Reject;
                }
                req.erc721.token = p.erc721_withdrawal.token;
                req.erc721.token_id = p.erc721_withdrawal.token_id;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC721, in.app_contract, req,
                    CMA_PARSER_ERC721_VOUCHER_PAYLOAD_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc1155(from_cma(p.erc1155_single_withdrawal.token),
                    from_cma(p.erc1155_single_withdrawal.token_id));
                if (!withdraw(asset, from_cma(p.erc1155_single_withdrawal.amount))) {
                    return Completion::Reject;
                }
                req.erc1155_single.token = p.erc1155_single_withdrawal.token;
                req.erc1155_single.token_id = p.erc1155_single_withdrawal.token_id;
                req.erc1155_single.amount = p.erc1155_single_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC1155_SINGLE, in.app_contract, req,
                    CMA_PARSER_ERC1155_SINGLE_VOUCHER_PAYLOAD_MIN_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_WITHDRAWAL: {
                auto &w = p.erc1155_batch_withdrawal;
                if (w.token_ids.length != w.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                Address token = from_cma(w.token);
                for (size_t t = 0; t < w.token_ids.length; ++t) {
                    U256 id;
                    U256 amount;
                    std::copy_n(w.token_ids.data[t], 32, id.b.begin());
                    std::copy_n(w.amounts.data[t], 32, amount.b.begin());
                    if (!withdraw(AssetRef::erc1155(token, id), amount)) {
                        return Completion::Reject;
                    }
                }
                req.erc1155_batch.token = w.token;
                req.erc1155_batch.token_ids = w.token_ids;
                req.erc1155_batch.amounts = w.amounts;
                size_t size = CMA_PARSER_ERC1155_BATCH_VOUCHER_PAYLOAD_MIN_SIZE + 2 * w.token_ids.length * CMA_ABI_U256_LENGTH;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC1155_BATCH, in.app_contract, req, size);
            }
            case CMA_PARSER_INPUT_TYPE_ETHER_TRANSFER:
                return do_transfer(sink, me, from_cma(p.ether_transfer.receiver), AssetRef::ether(), from_cma(p.ether_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC20_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc20_transfer.receiver), AssetRef::erc20(from_cma(p.erc20_transfer.token)),
                    from_cma(p.erc20_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC721_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc721_transfer.receiver),
                    AssetRef::erc721(from_cma(p.erc721_transfer.token), from_cma(p.erc721_transfer.token_id)), U256::from_u64(1));
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc1155_single_transfer.receiver),
                    AssetRef::erc1155(from_cma(p.erc1155_single_transfer.token), from_cma(p.erc1155_single_transfer.token_id)),
                    from_cma(p.erc1155_single_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_TRANSFER: {
                auto &t = p.erc1155_batch_transfer;
                if (t.token_ids.length != t.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                Address token = from_cma(t.token);
                for (size_t i = 0; i < t.token_ids.length; ++i) {
                    U256 id;
                    U256 amount;
                    std::copy_n(t.token_ids.data[i], 32, id.b.begin());
                    std::copy_n(t.amounts.data[i], 32, amount.b.begin());
                    Completion c = do_transfer(sink, me, from_cma(t.receiver), AssetRef::erc1155(token, id), amount);
                    if (c != Completion::Accept) {
                        return c;
                    }
                }
                return Completion::Accept;
            }
            default:
                return fail(sink, "unsupported libcma input type", static_cast<int>(p.type));
        }
    }

    Completion do_transfer(OutputSink &sink, const AccountRef &from, const U256 &to_id, const AssetRef &asset, const U256 &amount) {
        int lrc = ledger.transfer(from, AccountRef::of_id(to_id), asset, amount);
        if (lrc != 0) {
            return fail(sink, ledger.last_error(), lrc);
        }
        json rep{{"transfer", "ok"}, {"to", to_hex(to_id)}, {"amount", dec(amount)}};
        if (asset.kind != AssetKind::Base) {
            rep["token"] = to_hex(asset.token);
        }
        (void) report_json(sink, rep);
        return Completion::Accept;
    }

    // --- tester commands ---------------------------------------------------------------------
    Completion emit_check(OutputSink &sink, int rc, const char *what) {
        if (rc < 0) {
            return fail(sink, std::string("unable to emit ") + what, rc);
        }
        return Completion::Accept;
    }

    Completion run_command(const DirectInput &in, const Command &c, OutputSink &sink, TerminalArgs &term, int depth) {
        switch (c.cmd) {
            case Cmd::EmitNotices: {
                fill_pattern(scratch, c.size, c.pattern, in.index);
                for (uint32_t i = 0; i < c.count; ++i) {
                    if (int rc = sink.notice(scratch); rc < 0) {
                        return fail(sink, "unable to emit notice #" + std::to_string(i), rc);
                    }
                }
                return Completion::Accept;
            }
            case Cmd::EmitNotice:
                return emit_check(sink, sink.notice(c.data), "notice");
            case Cmd::EmitReports: {
                fill_pattern(scratch, c.size, c.pattern, in.index);
                for (uint32_t i = 0; i < c.count; ++i) {
                    if (int rc = sink.report(scratch); rc < 0) {
                        return fail(sink, "unable to emit report #" + std::to_string(i), rc);
                    }
                }
                return Completion::Accept;
            }
            case Cmd::EmitReport:
                return emit_check(sink, sink.report(c.data), "report");
            case Cmd::EmitVouchers:
                for (uint32_t i = 0; i < c.count; ++i) {
                    if (int rc = sink.voucher(c.addr, c.value, c.data); rc < 0) {
                        return fail(sink, "unable to emit voucher #" + std::to_string(i), rc);
                    }
                }
                return Completion::Accept;
            case Cmd::EmitDelegateCallVoucher:
                return emit_check(sink, sink.delegate_call_voucher(c.addr, c.data), "delegate call voucher");
            case Cmd::EmitRawOutput:
                return emit_check(sink, sink.raw_output(c.data), "raw output");
            case Cmd::Echo: {
                json rep{{"echo", true}, {"sender", to_hex(in.sender)}, {"length", c.data.size()},
                    {"keccak256", keccak_hex(c.data)}, {"index", in.index}, {"block", in.block_number}};
                if (int rc = report_json(sink, rep); rc < 0) {
                    return fail(sink, "unable to emit echo report", rc);
                }
                return emit_check(sink, sink.notice(c.data), "echo notice");
            }
            case Cmd::Batch: {
                if (depth > 0) {
                    return fail(sink, "nested batch not allowed");
                }
                for (size_t i = 0; i < c.calls.size(); ++i) {
                    Completion r = run_payload(in, c.calls[i], sink, term, depth + 1);
                    if (r != Completion::Accept) {
                        log("batch stopped at call %zu with completion %d", i, static_cast<int>(r));
                        return r;
                    }
                }
                return Completion::Accept;
            }
            case Cmd::Reject:
                if (!c.data.empty()) {
                    (void) sink.report(c.data);
                }
                return Completion::Reject;
            case Cmd::RaiseException:
                term.payload.assign(c.data.begin(), c.data.end());
                return Completion::Exception;
            case Cmd::Halt:
                term.mode = c.mode;
                return Completion::Halt;
            case Cmd::ManualYield:
                term.yield_reason = c.reason;
                term.yield_data = c.data32;
                return Completion::UnexpectedYield;
            case Cmd::CorruptOutputsRoot:
                switch (c.mode) {
                    case 0:
                        return Completion::CorruptRootValue;
                    case 1:
                        return Completion::CorruptRootShort;
                    case 2:
                        return Completion::CorruptRootLong;
                    default:
                        return fail(sink, "corruptOutputsRoot: mode must be 0, 1 or 2");
                }
            case Cmd::BurnCycles: {
                uint64_t total = c.iterations;
                uint64_t done = 0;
                uint64_t chunk = c.pattern != 0 ? std::max<uint64_t>(total / 16, 1) : total;
                uint64_t acc = 0;
                while (done < total) {
                    uint64_t n = std::min(chunk, total - done);
                    acc ^= burn_cycles(n);
                    done += n;
                    if (c.pattern != 0) {
                        sink.progress(static_cast<uint32_t>(done * 100 / total));
                    }
                }
                json rep{{"burned", total}, {"checksum", acc}};
                (void) report_json(sink, rep);
                return Completion::Accept;
            }
            case Cmd::Crash:
                term.mode = c.mode;
                return Completion::Crash;
            case Cmd::SetDepositPolicy:
                if (c.mode > 2) {
                    return fail(sink, "setDepositPolicy: policy must be 0, 1 or 2");
                }
                cfg.deposit_policy = static_cast<DepositPolicy>(c.mode);
                (void) report_json(sink, json{{"deposit_policy", c.mode}});
                return Completion::Accept;
            case Cmd::WithdrawErc20Safe: {
                Address dest = c.addr.is_zero() ? cfg.safe_erc20_transfer : c.addr;
                if (dest.is_zero()) {
                    return fail(sink, "withdrawErc20Safe: no SafeErc20Transfer address (argument or QA_SAFE_ERC20_TRANSFER)");
                }
                AccountRef me = AccountRef::of_wallet(in.sender);
                int lrc = ledger.withdraw(me, AssetRef::erc20(c.addr2), c.value);
                if (lrc != 0) {
                    return fail(sink, ledger.last_error(), lrc);
                }
                abi::Writer w;
                w.selector(abi::selector("safeTransfer(address,address,uint256)"));
                w.address(c.addr2);
                w.address(in.sender);
                w.word(c.value);
                bytes payload = w.finish();
                return emit_check(sink, sink.delegate_call_voucher(dest, payload), "delegate call voucher");
            }
            case Cmd::Progress:
                sink.progress(c.data32);
                return Completion::Accept;
            case Cmd::Count_:
                break;
        }
        return fail(sink, "unreachable command");
    }

    Completion run_payload(const DirectInput &in, bytes_view payload, OutputSink &sink, TerminalArgs &term, int depth) {
        Command c;
        std::string err;
        DecodeStatus st = decode_command(payload, c, err);
        switch (st) {
            case DecodeStatus::Ok:
                return run_command(in, c, sink, term, depth);
            case DecodeStatus::Unknown: {
                DirectInput sub = in;
                sub.payload = payload;
                return handle_cma_advance(sub, sink);
            }
            case DecodeStatus::Malformed:
            case DecodeStatus::Empty:
                return fail(sink, err);
        }
        return fail(sink, "unreachable decode status");
    }

    // --- inspect ------------------------------------------------------------------------------
    json state_json() {
        bytes_view drive = ledger.accounts_drive();
        return json{{"executed_input_count", prog.executed_input_count}, {"last_block", prog.last_block},
            {"rejected_count", rejected_count}, {"deposit_policy", static_cast<int>(cfg.deposit_policy)},
            {"state_backend", ledger.is_nvram() ? "nvram" : "file"},
            {"accounts_drive_length", drive.size()},
            {"accounts_drive_keccak256", drive.empty() ? std::string("unavailable") : keccak_hex(drive)}};
    }

    Completion handle_inspect(bytes_view payload, OutputSink &sink, TerminalArgs &term) {
        static const std::string STATE = "state";
        bool is_json = !payload.empty() && payload[0] == '{' && payload.size() <= MAX_INSPECT_JSON;
        if (!is_json) {
            if (payload.size() == STATE.size() && std::equal(payload.begin(), payload.end(), STATE.begin())) {
                return emit_check(sink, report_json(sink, state_json()), "state report");
            }
            // raw echo (INS-001: 2 MiB payloads)
            return emit_check(sink, sink.report(payload), "echo report");
        }
        json q;
        try {
            q = json::parse(payload.begin(), payload.end());
        } catch (const std::exception &e) {
            return fail(sink, std::string("invalid json: ") + e.what());
        }
        std::string method = q.value("method", "");
        json params = q.contains("params") ? q["params"] : json::array();
        auto param_u64 = [&](size_t i, uint64_t def) -> uint64_t {
            if (!params.is_array() || params.size() <= i) {
                return def;
            }
            if (params[i].is_number_unsigned()) {
                return params[i].get<uint64_t>();
            }
            if (params[i].is_string()) {
                U256 v;
                if (parse_u256(params[i].get<std::string>(), v) && v.fits_u64()) {
                    return v.to_u64();
                }
            }
            return def;
        };
        if (method == "qa_info") {
            json j = state_json();
            j["config"] = json::parse(cfg.describe_json());
            j["selectors"] = json::parse(selectors_json());
            return emit_check(sink, report_json(sink, j), "info report");
        }
        if (method == "qa_state") {
            return emit_check(sink, report_json(sink, state_json()), "state report");
        }
        if (method == "qa_echo") {
            bytes data;
            if (params.is_array() && !params.empty() && params[0].is_string()) {
                std::string s = params[0].get<std::string>();
                if (!from_hex(s, data)) {
                    data.assign(s.begin(), s.end());
                }
            }
            return emit_check(sink, sink.report(data), "echo report");
        }
        if (method == "qa_reports") {
            uint64_t count = param_u64(0, 1);
            uint64_t size = param_u64(1, 32);
            uint8_t pattern = static_cast<uint8_t>(param_u64(2, 0));
            fill_pattern(scratch, static_cast<size_t>(size), pattern, 0);
            for (uint64_t i = 0; i < count; ++i) {
                if (int rc = sink.report(scratch); rc < 0) {
                    return fail(sink, "unable to emit inspect report #" + std::to_string(i), rc);
                }
            }
            return Completion::Accept;
        }
        if (method == "qa_exception") {
            std::string s = (params.is_array() && !params.empty() && params[0].is_string()) ? params[0].get<std::string>()
                                                                                              : "inspect exception";
            if (!from_hex(s, term.payload)) {
                term.payload.assign(s.begin(), s.end());
            }
            return Completion::Exception;
        }
        if (method == "qa_reject") {
            (void) report_json(sink, json{{"rejected", true}});
            return Completion::Reject;
        }
        if (method == "qa_burn") {
            uint64_t n = param_u64(0, 1000000);
            uint64_t acc = burn_cycles(n);
            return emit_check(sink, report_json(sink, json{{"burned", n}, {"checksum", acc}}), "burn report");
        }
        // libcma inspects: ledger_getBalance / ledger_getTotalSupply
        cmt_rollup_inspect_t ins{};
        ins.payload.length = payload.size();
        ins.payload.data = const_cast<uint8_t *>(payload.data());
        cma_parser_input_t p{};
        int rc = cma_parser_decode_inspect(CMA_PARSER_INPUT_TYPE_AUTO, &ins, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("unknown inspect: ") + cma_parser_get_last_error_message(), rc);
        }
        U256 out;
        int lrc = 0;
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT:
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), AssetRef::ether(), out);
                break;
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT_TOKEN_ADDRESS:
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), AssetRef::erc20(from_cma(p.balance.token)), out);
                break;
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT_TOKEN_ADDRESS_ID: {
                AssetRef a = AssetRef::erc721(from_cma(p.balance.token), from_cma(p.balance.token_id));
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), a, out);
                if (lrc != 0) { // maybe an ERC-1155 asset
                    a.kind = AssetKind::TokenIdAmount;
                    lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), a, out);
                }
                break;
            }
            case CMA_PARSER_INPUT_TYPE_SUPPLY:
                lrc = ledger.total_supply(AssetRef::ether(), out);
                break;
            case CMA_PARSER_INPUT_TYPE_SUPPLY_TOKEN_ADDRESS:
                lrc = ledger.total_supply(AssetRef::erc20(from_cma(p.supply.token)), out);
                break;
            case CMA_PARSER_INPUT_TYPE_SUPPLY_TOKEN_ADDRESS_ID: {
                AssetRef a = AssetRef::erc721(from_cma(p.supply.token), from_cma(p.supply.token_id));
                lrc = ledger.total_supply(a, out);
                if (lrc != 0) {
                    a.kind = AssetKind::TokenIdAmount;
                    lrc = ledger.total_supply(a, out);
                }
                break;
            }
            default:
                return fail(sink, "unsupported inspect type", static_cast<int>(p.type));
        }
        if (lrc != 0) {
            return fail(sink, ledger.last_error(), lrc);
        }
        return emit_check(sink, sink.report(bytes_view(out.b.data(), out.b.size())), "balance report");
    }
};

Engine::Engine(Config cfg, const std::string &state_file, const std::string &ledger_file)
    : m(std::make_unique<Impl>(cfg, state_file, ledger_file)) {}
Engine::~Engine() = default;

Completion Engine::execute_direct_input(const DirectInput &input, OutputSink &sink, TerminalArgs &terminal) {
    terminal = TerminalArgs{};
    m->current_index = input.index;
    Completion c = Completion::Reject;
    try {
        cma_parser_input_type_t kind = m->portal_kind(input.sender);
        if (kind != CMA_PARSER_INPUT_TYPE_NONE) {
            c = m->handle_deposit(input, kind, sink);
        } else {
            c = m->run_payload(input, input.payload, sink, terminal, 0);
        }
    } catch (const std::exception &e) {
        c = m->fail(sink, std::string("internal error: ") + e.what());
    } catch (...) {
        c = m->fail(sink, "internal error");
    }
    if (c == Completion::Accept) {
        m->prog.executed_input_count += 1;
        m->prog.last_block = std::max(m->prog.last_block, input.block_number);
    } else if (c == Completion::Reject) {
        m->rejected_count += 1;
    }
    return c;
}

Completion Engine::inspect(bytes_view payload, OutputSink &sink, TerminalArgs &terminal) {
    terminal = TerminalArgs{};
    try {
        return m->handle_inspect(payload, sink, terminal);
    } catch (const std::exception &e) {
        return m->fail(sink, std::string("internal error: ") + e.what());
    } catch (...) {
        return m->fail(sink, "internal error");
    }
}

Progress Engine::progress() const {
    return m->prog;
}

const Config &Engine::config() const {
    return m->cfg;
}

Ledger &Engine::ledger() {
    return m->ledger;
}

void Engine::flush() {
    m->ledger.flush();
}

} // namespace qatester
