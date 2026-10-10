// Tester commands carried in advance inputs (sender != portal). Wire format: Solidity ABI call
// (4-byte selector + encoded arguments). Selectors are keccak256 of the signatures below.
#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace qatester {

enum class Cmd : uint8_t {
    EmitNotices,             // emitNotices(uint32 count,uint32 size,uint8 pattern)
    EmitNotice,              // emitNotice(bytes payload)
    EmitReports,             // emitReports(uint32 count,uint32 size,uint8 pattern)
    EmitReport,              // emitReport(bytes payload)
    EmitVouchers,            // emitVouchers(uint32 count,address destination,uint256 value,bytes payload)
    EmitDelegateCallVoucher, // emitDelegateCallVoucher(address destination,bytes payload)
    EmitRawOutput,           // emitRawOutput(bytes output)
    Echo,                    // echo(bytes data)
    Batch,                   // batch(bytes[] calls)
    Reject,                  // reject(bytes report)
    RaiseException,          // raiseException(bytes payload)
    Halt,                    // halt(uint8 mode)
    ManualYield,             // manualYield(uint16 reason,uint32 data)
    CorruptOutputsRoot,      // corruptOutputsRoot(uint8 mode)
    BurnCycles,              // burnCycles(uint64 iterations,uint8 progress)
    Crash,                   // crash(uint8 kind)
    SetDepositPolicy,        // setDepositPolicy(uint8 policy)
    WithdrawErc20Safe,       // withdrawErc20Safe(address safeErc20Transfer,address token,uint256 amount,bytes data)
    Progress,                // progress(uint32 value)
    Count_,
};

struct Command {
    Cmd cmd{};
    uint32_t count{};
    uint32_t size{};
    uint8_t pattern{};
    uint8_t mode{};
    uint16_t reason{};
    uint32_t data32{};
    uint64_t iterations{};
    Address addr;   // destination / safeErc20Transfer
    Address addr2;  // token
    U256 value;
    bytes_view data;
    std::vector<bytes_view> calls; // batch
};

[[nodiscard]] std::string_view signature_of(Cmd cmd);
[[nodiscard]] uint32_t selector_of(Cmd cmd);
[[nodiscard]] std::string selectors_json(); // {"name":"0x...",...}

enum class DecodeStatus : uint8_t { Ok, Unknown, Malformed, Empty };

// Decodes one command. `calls` views point into `payload`.
[[nodiscard]] DecodeStatus decode_command(bytes_view payload, Command &out, std::string &err);

} // namespace qatester
