/* qa-tester extensions next to the sequencer's application-engine.h seam. Nothing here is part of
 * the seam: the sequencer host never calls these. They serve the genesis tool, the sequencer guest
 * driver (src/seq_main.cpp), which owns the rollup device and needs the engine's reports, raw and
 * delegate-call outputs, inspects and the per-input metadata the seam does not carry, and the tests.
 * Status codes follow application-engine.h (0 ok, negative failure) and leave the reason in
 * application_engine_get_last_error_message(). */
#ifndef QATESTER_SEQ_EXT_H
#define QATESTER_SEQ_EXT_H

#include "application-engine.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a genesis state into `state_path` (a file, created or grown, or the machine's NVRAM).
 * Configuration comes from the QA_* environment like the drivers'; `token` (NULL or all zero: the
 * environment's / devnet default) is the accounts-drive ERC-20, pinned in the state together with
 * the batch submitter. Refuses an initialised state. */
int qatester_genesis(const char *state_path, const uint8_t sequencer[20], const uint8_t token[20]);

/* The functions below act on the live engine: the one application_engine_from_dump opened over a
 * character device, the machine's NVRAM (there is exactly one in the guest; the host never opens
 * one). They return -ENOENT while no live engine exists. */

/* Where the engine sends what cannot cross the seam. Every callback may be NULL (dropped with a log
 * line). Callbacks return 0 or a negative errno; the engine treats a failure like a device error. */
typedef struct qatester_aux_sink {
    void *ctx;
    int (*report)(void *ctx, const uint8_t *data, uint64_t size);
    int (*raw_output)(void *ctx, const uint8_t *data, uint64_t size);
    int (*delegate_call_voucher)(void *ctx, const uint8_t destination[20], const uint8_t *data, uint64_t size);
    void (*progress)(void *ctx, uint32_t value);
} qatester_aux_sink;

int qatester_live_attach_aux(const qatester_aux_sink *aux);

/* Input metadata the seam does not carry (EvmAdvance fields). Used for reports and output
 * encoding only (ERC-721/1155 withdrawal vouchers name the app contract); the state never depends
 * on it. Stays in effect until replaced; zero on the host. */
typedef struct qatester_input_context {
    uint64_t block_timestamp;
    uint64_t index;
    uint64_t chain_id;
    uint8_t app_contract[20];
} qatester_input_context;

int qatester_live_set_input_context(const qatester_input_context *context);

/* Scheduler facts shown by the `state` inspect report. */
int qatester_live_set_scheduler_status(uint64_t next_batch_nonce, uint64_t queued_directs);

/* Runs an inspect query; reports go to `report` in order. Terminal completions (qa_exception,
 * qa_reject) become a report, since the scheduler guest never leaves the accepted state. */
int qatester_live_inspect(const uint8_t *query, uint64_t size,
    int (*report)(void *ctx, const uint8_t *data, uint64_t size), void *ctx);

/* Test hook: makes any engine (a host dump) the live one, so the extensions can be exercised
 * without a machine. NULL clears it. */
void qatester_set_live_engine(ApplicationEngine *engine);

/* The fee port (include/qatester/fee.h): 0 with the big-endian amount in `out`, -1 when the
 * exponent exceeds qatester_fee_max_exponent(). */
int qatester_fee_to_linear(uint16_t exponent, uint8_t out[32]);
uint16_t qatester_fee_max_exponent(void);

#ifdef __cplusplus
}
#endif

#endif
