// qa-tester: rollup driver. Owns the rollup device (libcmt) and nothing else; every decision is
// made by qatester::Engine. See README.md for the command catalog and the terminal-state recipes.
#include "cmt_sink.h"
#include "qatester/config.h"
#include "qatester/engine.h"
#include "qatester/sink.h"
#include "qatester/types.h"

extern "C" {
#include <libcmt/abi.h>
#include <libcmt/io.h>
#include <libcmt/merkle.h>
#include <libcmt/rollup.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>
}

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace qatester;

namespace {

const char *g_merkle_path = "/tmp/merkle";
bool g_log = true;

__attribute__((format(printf, 1, 2))) void logf(const char *fmt, ...) {
    if (!g_log) {
        return;
    }
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[qa-tester] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

void save_merkle(cmt_rollup_t *r) {
    mode_t old = umask(0000);
    int rc = cmt_rollup_save_merkle(r, g_merkle_path);
    umask(old);
    if (rc != 0) {
        logf("warning: unable to save merkle tree: %s", std::strerror(-rc));
    }
}

// Manual RX_ACCEPTED yield with a hand-written TX payload (the outputs root, possibly corrupted).
// Returns the next request type (>= 0) or a negative errno.
int manual_yield(cmt_rollup_t *r, uint16_t reason, const uint8_t *data, uint32_t length, uint32_t *next_length) {
    cmt_buf_t tx = cmt_io_get_tx(r->io);
    if (length > cmt_buf_length(&tx)) {
        return -ENOBUFS;
    }
    if (length > 0 && data != nullptr) {
        std::memcpy(tx.begin, data, length);
    }
    cmt_io_yield_t req{HTIF_DEVICE_YIELD, HTIF_YIELD_CMD_MANUAL, reason, length};
    int rc = cmt_io_yield(r->io, &req);
    if (rc != 0) {
        return rc;
    }
    *next_length = req.data;
    return req.reason;
}

int corrupt_root_yield(cmt_rollup_t *r, Completion c, uint32_t *next_length) {
    uint8_t buf[33];
    std::memset(buf, 0, sizeof buf);
    cmt_merkle_get_root_hash(r->merkle, buf);
    uint32_t len = 32;
    switch (c) {
        case Completion::CorruptRootValue:
            for (auto &x : buf) {
                x ^= 0xffU; // every byte flipped: certainly not the real root
            }
            break;
        case Completion::CorruptRootShort:
            len = 31;
            break;
        case Completion::CorruptRootLong:
            len = 33;
            break;
        default:
            break;
    }
    logf("yielding RX_ACCEPTED with a corrupted outputs root (mode %d, %u bytes)", static_cast<int>(c), len);
    return manual_yield(r, HTIF_YIELD_MANUAL_REASON_RX_ACCEPTED, buf, len, next_length);
}

[[noreturn]] void do_halt(uint8_t mode) {
    logf("halting the machine (mode %u)", mode);
    sync();
    switch (mode) {
        case 1:
            reboot(RB_POWER_OFF);
            logf("reboot(RB_POWER_OFF) failed: %s; exiting instead", std::strerror(errno));
            std::exit(0);
        case 2:
            std::abort();
        default:
            std::exit(0);
    }
}

[[noreturn]] void do_crash(uint8_t kind) {
    logf("crashing the guest process (kind %u)", kind);
    sync();
    switch (kind) {
        case 1: {
            volatile int *p = nullptr;
            *p = 1; // NOLINT: intentional segfault
            std::abort();
        }
        case 2:
            std::exit(3);
        default:
            throw std::runtime_error("qa-tester: intentional unhandled exception (crash kind 0)");
    }
}

} // namespace

int main(int argc, char *argv[]) {
    if (std::setvbuf(stderr, nullptr, _IONBF, 0) != 0) {
        return 1;
    }
    Config cfg = Config::devnet_defaults();
    std::string err;
    if (!cfg.apply_env(err)) {
        std::fprintf(stderr, "[qa-tester] bad configuration: %s\n", err.c_str());
        return 1;
    }
    g_log = cfg.log;
    // argv: [state range]. In the machine it is the `state` NVRAM (/dev/uio0, see cartesi.toml);
    // on the host a plain file. A blank state gets its genesis on the first boot.
    const char *state = argc > 1 ? argv[1] : std::getenv("QA_STATE_DEVICE");
    if (state == nullptr || *state == '\0') {
        state = "/dev/uio0";
    }
    if (const char *mp = std::getenv("QA_MERKLE_PATH"); mp != nullptr && *mp != '\0') {
        g_merkle_path = mp;
    }
    logf("config %s", cfg.describe_json().c_str());
    logf("state range %s", state);

    Engine *engine = nullptr;
    try {
        if (!Engine::is_initialized(cfg, state)) {
            logf("blank state: writing the genesis");
            Engine::genesis(cfg, state, Address{});
        }
        engine = new Engine(cfg, state);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[qa-tester] unable to initialise the ledger: %s\n", e.what());
        return 1;
    }

    cmt_rollup_t rollup{};
    int rc = cmt_rollup_init(&rollup);
    if (rc != 0) {
        std::fprintf(stderr, "[qa-tester] unable to initialise the rollup device: %s\n", std::strerror(-rc));
        return 1;
    }
    struct stat st{};
    if (stat(g_merkle_path, &st) == 0) {
        rc = cmt_rollup_load_merkle(&rollup, g_merkle_path);
        if (rc != 0) {
            std::fprintf(stderr, "[qa-tester] unable to load merkle tree: %s\n", std::strerror(-rc));
            return 1;
        }
    }
    sync(); // a flash-drive state goes through the page cache; an NVRAM does not need this

    CmtSink sink(&rollup);
    Completion pending = Completion::Accept;
    TerminalArgs term;
    logf("processing rollup requests...");
    while (true) {
        int next_type = 0;
        uint32_t next_len = 0;
        switch (pending) {
            case Completion::Accept: {
                save_merkle(&rollup);
                cmt_rollup_finish_t fin{};
                fin.accept_previous_request = true;
                rc = cmt_rollup_finish(&rollup, &fin);
                if (rc != 0) {
                    std::fprintf(stderr, "[qa-tester] rollup finish failed: %s\n", std::strerror(-rc));
                    return 1;
                }
                next_type = fin.next_request_type;
                next_len = fin.next_request_payload_length;
                break;
            }
            case Completion::Reject:
                // On the real machine this yield never returns: the host restores the snapshot taken
                // at the previous RX_ACCEPTED yield and the guest resumes from there. The libcmt mock
                // does return, with the next request in the reply, which is what the e2e test uses.
                next_type = manual_yield(&rollup, HTIF_YIELD_MANUAL_REASON_RX_REJECTED, nullptr, 0, &next_len);
                break;
            case Completion::Exception: {
                logf("raising a guest exception (%zu bytes)", term.payload.size());
                cmt_abi_bytes_t b{term.payload.size(), term.payload.data()};
                rc = cmt_rollup_emit_exception(&rollup, &b);
                if (rc != 0) {
                    std::fprintf(stderr, "[qa-tester] exception yield failed: %s\n", std::strerror(-rc));
                    return 1;
                }
                // The machine is at a fixed point in production; only the mock driver gets here.
                pending = Completion::Accept;
                continue;
            }
            case Completion::Halt:
                do_halt(term.mode);
            case Completion::Crash:
                do_crash(term.mode);
            case Completion::UnexpectedYield:
                logf("manual yield with reason %u data %u", term.yield_reason, term.yield_data);
                save_merkle(&rollup);
                next_type = manual_yield(&rollup, term.yield_reason, nullptr, term.yield_data, &next_len);
                break;
            case Completion::CorruptRootValue:
            case Completion::CorruptRootShort:
            case Completion::CorruptRootLong:
                save_merkle(&rollup);
                next_type = corrupt_root_yield(&rollup, pending, &next_len);
                break;
        }
        if (next_type < 0) {
            std::fprintf(stderr, "[qa-tester] yield failed: %s\n", std::strerror(-next_type));
            return 1;
        }

        if (next_type == HTIF_YIELD_REASON_ADVANCE_STATE) {
            cmt_rollup_advance_t adv{};
            rc = cmt_rollup_read_advance_state(&rollup, &adv);
            if (rc != 0) {
                std::fprintf(stderr, "[qa-tester] unable to read advance state: %s\n", std::strerror(-rc));
                return 1;
            }
            DirectInput in;
            std::memcpy(in.sender.b.data(), adv.msg_sender.data, 20);
            std::memcpy(in.app_contract.b.data(), adv.app_contract.data, 20);
            in.block_number = adv.block_number;
            in.block_timestamp = adv.block_timestamp;
            in.index = adv.index;
            in.chain_id = adv.chain_id;
            in.payload = bytes_view(static_cast<const uint8_t *>(adv.payload.data), adv.payload.length);
            logf("advance #%llu from %s (%zu bytes)", static_cast<unsigned long long>(adv.index),
                to_hex(in.sender).c_str(), in.payload.size());
            pending = engine->execute_direct_input(in, sink, term);
            if (pending == Completion::Accept) {
                engine->commit_progress(in.block_number);
            } else if (pending == Completion::Reject) {
                engine->note_reject();
            }
            engine->flush();
            sync();
            logf("advance #%llu completion %d", static_cast<unsigned long long>(adv.index), static_cast<int>(pending));
        } else if (next_type == HTIF_YIELD_REASON_INSPECT_STATE) {
            cmt_rollup_inspect_t ins{};
            rc = cmt_rollup_read_inspect_state(&rollup, &ins);
            if (rc != 0) {
                std::fprintf(stderr, "[qa-tester] unable to read inspect state: %s\n", std::strerror(-rc));
                return 1;
            }
            bytes_view q(static_cast<const uint8_t *>(ins.payload.data), ins.payload.length);
            logf("inspect (%zu bytes)", q.size());
            pending = engine->inspect(q, sink, term);
            (void) next_len;
            logf("inspect completion %d", static_cast<int>(pending));
        } else {
            std::fprintf(stderr, "[qa-tester] unknown request type %d\n", next_type);
            return 1;
        }
    }
}
