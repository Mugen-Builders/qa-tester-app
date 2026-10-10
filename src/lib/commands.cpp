#include "qatester/commands.h"

#include "qatester/abi.h"

#include <array>
#include <sstream>

namespace qatester {

namespace {

constexpr size_t MAX_BATCH_CALLS = 1024;

struct Entry {
    Cmd cmd;
    std::string_view signature;
    std::string_view name;
};

constexpr std::array<Entry, static_cast<size_t>(Cmd::Count_)> TABLE = {{
    {Cmd::EmitNotices, "emitNotices(uint32,uint32,uint8)", "emitNotices"},
    {Cmd::EmitNotice, "emitNotice(bytes)", "emitNotice"},
    {Cmd::EmitReports, "emitReports(uint32,uint32,uint8)", "emitReports"},
    {Cmd::EmitReport, "emitReport(bytes)", "emitReport"},
    {Cmd::EmitVouchers, "emitVouchers(uint32,address,uint256,bytes)", "emitVouchers"},
    {Cmd::EmitDelegateCallVoucher, "emitDelegateCallVoucher(address,bytes)", "emitDelegateCallVoucher"},
    {Cmd::EmitRawOutput, "emitRawOutput(bytes)", "emitRawOutput"},
    {Cmd::Echo, "echo(bytes)", "echo"},
    {Cmd::Batch, "batch(bytes[])", "batch"},
    {Cmd::Reject, "reject(bytes)", "reject"},
    {Cmd::RaiseException, "raiseException(bytes)", "raiseException"},
    {Cmd::Halt, "halt(uint8)", "halt"},
    {Cmd::ManualYield, "manualYield(uint16,uint32)", "manualYield"},
    {Cmd::CorruptOutputsRoot, "corruptOutputsRoot(uint8)", "corruptOutputsRoot"},
    {Cmd::BurnCycles, "burnCycles(uint64,uint8)", "burnCycles"},
    {Cmd::Crash, "crash(uint8)", "crash"},
    {Cmd::SetDepositPolicy, "setDepositPolicy(uint8)", "setDepositPolicy"},
    {Cmd::WithdrawErc20Safe, "withdrawErc20Safe(address,address,uint256,bytes)", "withdrawErc20Safe"},
    {Cmd::Progress, "progress(uint32)", "progress"},
}};

const std::array<uint32_t, TABLE.size()> &selectors() {
    static const std::array<uint32_t, TABLE.size()> sels = [] {
        std::array<uint32_t, TABLE.size()> s{};
        for (size_t i = 0; i < TABLE.size(); ++i) {
            s[i] = abi::selector(TABLE[i].signature);
        }
        return s;
    }();
    return sels;
}

} // namespace

std::string_view signature_of(Cmd cmd) {
    return TABLE[static_cast<size_t>(cmd)].signature;
}

uint32_t selector_of(Cmd cmd) {
    return selectors()[static_cast<size_t>(cmd)];
}

std::string selectors_json() {
    std::ostringstream o;
    o << '{';
    for (size_t i = 0; i < TABLE.size(); ++i) {
        if (i != 0) {
            o << ',';
        }
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%08x", selectors()[i]);
        o << '"' << TABLE[i].name << "\":{\"selector\":\"" << hex << "\",\"signature\":\"" << TABLE[i].signature
          << "\"}";
    }
    o << '}';
    return o.str();
}

DecodeStatus decode_command(bytes_view payload, Command &out, std::string &err) {
    out = Command{};
    if (payload.empty()) {
        err = "empty payload";
        return DecodeStatus::Empty;
    }
    abi::Reader rd(payload);
    uint32_t sel = 0;
    if (!rd.read_selector(sel)) {
        err = "payload shorter than a selector";
        return DecodeStatus::Malformed;
    }
    const auto &sels = selectors();
    size_t idx = TABLE.size();
    for (size_t i = 0; i < sels.size(); ++i) {
        if (sels[i] == sel) {
            idx = i;
            break;
        }
    }
    if (idx == TABLE.size()) {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%08x", sel);
        err = std::string("unknown selector ") + hex;
        return DecodeStatus::Unknown;
    }
    out.cmd = TABLE[idx].cmd;
    bool ok = true;
    uint64_t tmp = 0;
    switch (out.cmd) {
        case Cmd::EmitNotices:
        case Cmd::EmitReports:
            ok = rd.uint_fits(tmp, 32);
            out.count = static_cast<uint32_t>(tmp);
            ok = ok && rd.uint_fits(tmp, 32);
            out.size = static_cast<uint32_t>(tmp);
            ok = ok && rd.uint_fits(tmp, 8);
            out.pattern = static_cast<uint8_t>(tmp);
            break;
        case Cmd::EmitNotice:
        case Cmd::EmitReport:
        case Cmd::EmitRawOutput:
        case Cmd::Echo:
        case Cmd::Reject:
        case Cmd::RaiseException:
            ok = rd.dyn_bytes(out.data);
            break;
        case Cmd::EmitVouchers:
            ok = rd.uint_fits(tmp, 32);
            out.count = static_cast<uint32_t>(tmp);
            ok = ok && rd.address(out.addr) && rd.word(out.value) && rd.dyn_bytes(out.data);
            break;
        case Cmd::EmitDelegateCallVoucher:
            ok = rd.address(out.addr) && rd.dyn_bytes(out.data);
            break;
        case Cmd::Batch:
            ok = rd.dyn_bytes_array(out.calls, MAX_BATCH_CALLS);
            break;
        case Cmd::Halt:
        case Cmd::CorruptOutputsRoot:
        case Cmd::Crash:
        case Cmd::SetDepositPolicy:
            ok = rd.uint_fits(tmp, 8);
            out.mode = static_cast<uint8_t>(tmp);
            break;
        case Cmd::ManualYield:
            ok = rd.uint_fits(tmp, 16);
            out.reason = static_cast<uint16_t>(tmp);
            ok = ok && rd.uint_fits(tmp, 32);
            out.data32 = static_cast<uint32_t>(tmp);
            break;
        case Cmd::BurnCycles:
            ok = rd.uint_fits(out.iterations, 64) && rd.uint_fits(tmp, 8);
            out.pattern = static_cast<uint8_t>(tmp); // progress flag
            break;
        case Cmd::WithdrawErc20Safe:
            ok = rd.address(out.addr) && rd.address(out.addr2) && rd.word(out.value) && rd.dyn_bytes(out.data);
            break;
        case Cmd::Progress:
            ok = rd.uint_fits(tmp, 32);
            out.data32 = static_cast<uint32_t>(tmp);
            break;
        case Cmd::Count_:
            ok = false;
            break;
    }
    if (!ok) {
        err = std::string("malformed arguments for ") + std::string(TABLE[idx].name);
        return DecodeStatus::Malformed;
    }
    return DecodeStatus::Ok;
}

} // namespace qatester
