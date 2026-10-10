// Output sink: how the engine hands outputs and reports to whoever owns the rollup device.
// Streaming (rather than returning a vector) is what lets a 2 MiB notice or 20k reports work
// without buffering, and lets an emission failure (-ENOBUFS) be observed by the engine.
// A queueing sink is all the future application-engine.h shim needs for drain_output().
#pragma once

#include "types.h"

namespace qatester {

class OutputSink {
public:
    virtual ~OutputSink() = default;
    // Each returns 0 on success or a negative errno (libcmt convention).
    virtual int notice(bytes_view payload) = 0;
    virtual int voucher(const Address &destination, const U256 &value, bytes_view payload) = 0;
    virtual int delegate_call_voucher(const Address &destination, bytes_view payload) = 0;
    virtual int raw_output(bytes_view output) = 0; // the whole output as-is (no ABI framing)
    virtual int report(bytes_view payload) = 0;
    virtual void progress(uint32_t value) = 0;
};

} // namespace qatester
