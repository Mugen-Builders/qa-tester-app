// The input-processing engine. Shaped after the sequencer's application-engine.h: a direct
// input goes in, outputs stream out through a sink, the state lives in one file, progress is a
// (count, clock) pair. The rollup device is never touched here; see src/main.cpp.
#pragma once

#include "config.h"
#include "ledger.h"
#include "sink.h"
#include "types.h"

#include <memory>
#include <string>

namespace qatester {

class Engine {
public:
    Engine(Config cfg, const std::string &state_file, const std::string &ledger_file = ""); // throws std::runtime_error
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Never throws. Emits through `sink`; fills `terminal` for non Accept/Reject completions.
    Completion execute_direct_input(const DirectInput &input, OutputSink &sink, TerminalArgs &terminal);
    Completion inspect(bytes_view payload, OutputSink &sink, TerminalArgs &terminal);

    [[nodiscard]] Progress progress() const;
    [[nodiscard]] const Config &config() const;
    [[nodiscard]] Ledger &ledger();
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Pattern generator shared by notices/reports: byte i of a payload with `pattern`.
void fill_pattern(bytes &buf, size_t size, uint8_t pattern, uint64_t seed);
// Deterministic CPU burn; returns a value depending on `iterations` so it is not optimised away.
uint64_t burn_cycles(uint64_t iterations);

} // namespace qatester
