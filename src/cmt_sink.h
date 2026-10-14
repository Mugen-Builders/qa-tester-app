// CmtSink: streams engine emissions straight into the rollup device (libcmt). Shared by the
// classic driver (src/main.cpp) and the sequencer guest driver (src/seq_main.cpp).
#pragma once

#include "qatester/sink.h"
#include "qatester/types.h"

extern "C" {
#include <libcmt/abi.h>
#include <libcmt/io.h>
#include <libcmt/merkle.h>
#include <libcmt/rollup.h>
}

#include <cerrno>
#include <cstring>

namespace qatester {

// Streams engine emissions straight into the rollup device.
class CmtSink final : public OutputSink {
public:
    explicit CmtSink(cmt_rollup_t *r) : m_r(r) {}

    int notice(bytes_view payload) override {
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_notice(m_r, &b, nullptr);
    }
    int voucher(const Address &destination, const U256 &value, bytes_view payload) override {
        cmt_abi_address_t a{};
        cmt_abi_u256_t v{};
        std::memcpy(a.data, destination.b.data(), 20);
        std::memcpy(v.data, value.b.data(), 32);
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_voucher(m_r, &a, &v, &b, nullptr);
    }
    int delegate_call_voucher(const Address &destination, bytes_view payload) override {
        cmt_abi_address_t a{};
        std::memcpy(a.data, destination.b.data(), 20);
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_delegate_call_voucher(m_r, &a, &b, nullptr);
    }
    // An output with arbitrary bytes: written to the TX buffer as-is and accounted in the outputs
    // Merkle tree exactly like libcmt does for vouchers and notices.
    int raw_output(bytes_view output) override {
        cmt_buf_t tx = cmt_io_get_tx(m_r->io);
        if (output.size() > cmt_buf_length(&tx)) {
            return -ENOBUFS;
        }
        std::memcpy(tx.begin, output.data(), output.size());
        cmt_io_yield_t req{HTIF_DEVICE_YIELD, HTIF_YIELD_CMD_AUTOMATIC, HTIF_YIELD_AUTOMATIC_REASON_TX_OUTPUT,
            static_cast<uint32_t>(output.size())};
        int rc = cmt_io_yield(m_r->io, &req);
        if (rc != 0) {
            return rc;
        }
        return cmt_merkle_push_back_data(m_r->merkle, output.size(), tx.begin);
    }
    int report(bytes_view payload) override {
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_report(m_r, &b);
    }
    void progress(uint32_t value) override {
        (void) cmt_rollup_progress(m_r, value);
    }

private:
    cmt_rollup_t *m_r;
};

} // namespace qatester
