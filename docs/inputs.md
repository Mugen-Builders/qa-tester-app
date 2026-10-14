# Sending inputs, deposits and inspects

Advance inputs are ABI calldata; Foundry's `cast` encodes them and the Cartesi CLI (or `cast send`)
delivers them. The tests use exactly the same `cast` invocations (`tests/machine/harness.py`,
`tests/host/mock_e2e.sh`), so what is written here is what is exercised.

## Encode

```sh
cast calldata 'emitNotices(uint32,uint32,uint8)' 1 2097056 0                     # OUT-002 boundary notice
cast calldata 'emitReports(uint32,uint32,uint8)' 20000 16 3                      # OUT-009
cast calldata 'emitVouchers(uint32,address,uint256,bytes)' 1 0x<dest> 0 0x<calldata>
cast calldata 'echo(bytes)' 0x$(xxd -p -c0 payload.bin)                          # INP-005: exact bytes back as a notice
cast calldata 'batch(bytes[])' "[$(cast calldata 'emitNotice(bytes)' 0x01),$(cast calldata 'corruptOutputsRoot(uint8)' 0)]"
cast calldata 'WithdrawErc20(address,uint256,bytes)' $TOKEN 1000000 0x           # libcma withdrawal -> Voucher(transfer)
cast calldata 'withdrawErc20Safe(address,address,uint256,bytes)' 0x<safeErc20Transfer> $TOKEN 1000000 0x
```

Numbers are decimal or `0x` hex; `bytes` arguments are `0x` hex (`0x` for empty).
`cast sig '<signature>'` prints a selector; the host unit tests cross-check every selector in the
C++ table against the same keccak.

## Send

```sh
cartesi send generic --input 0x…                                             # devnet, interactive account pick
cast send $INPUT_BOX 'addInput(address,bytes)' $APP 0x… --private-key $PK    # any chain
```

Deposits go through the portals, never through `addInput` directly:

```sh
cartesi deposit erc20 --token $TOKEN --amount 1000000 --exec-layer-data 0xdeadbeef
cast send $ERC20_PORTAL 'depositErc20Tokens(address,address,uint256,bytes)' $TOKEN $APP 1000000 0xdeadbeef --private-key $PK
```

When feeding the machine directly (the tests, `cartesi-machine --cmio-advance-state`), the portal
payload is `abi.encodePacked` and the input is the `EvmAdvance(...)` frame the InputBox would emit:

```sh
cast abi-encode --packed '(address,address,uint256,bytes)' $TOKEN $SENDER 1000000 0xdeadbeef     # ERC-20 portal payload
cast abi-encode --packed '(address,uint256,bytes)' $SENDER 5 0x                                   # Ether portal payload (empty execLayerData = 0x)
cast abi-encode --packed '(address,address,uint256,bytes)' $NFT $SENDER 9 \
    "$(cast abi-encode 'f(bytes,bytes)' 0x01 0xaa)"                                                # ERC-721: abi(baseLayerData, execLayerData)
cast calldata 'EvmAdvance(uint256,address,address,uint256,uint256,uint256,uint256,bytes)' \
    31337 $APP $SENDER $BLOCK $TIMESTAMP 0 $INDEX 0x<payload>                                      # chainId, app, sender, block, time, prevRandao, index
```

## Command catalog (advance inputs; sender is not a portal)

| Command | Effect | Completion | Catalog IDs |
|---|---|---|---|
| `emitNotices(uint32 count,uint32 size,uint8 pattern)` | `count` notices of `size` bytes; pattern 0 = `i & 0xff`, 1 = 0x00, 2 = 0xff, 3 = LCG(input index) | accept; reject with `{"error":"unable to emit notice #k","code":-105}` when the TX buffer overflows | OUT-001, OUT-002, OUT-006, OUT-009 |
| `emitNotice(bytes)` | one notice with that payload | accept | OUT-005, SMK-004 |
| `emitReports(uint32 count,uint32 size,uint8 pattern)` / `emitReport(bytes)` | reports | accept | OUT-004, OUT-009, INS-004 |
| `emitVouchers(uint32 count,address destination,uint256 value,bytes payload)` | `count` identical vouchers | accept | OUT-003, OUT-006, EGR-001/003/004/009, FOR-015 |
| `emitDelegateCallVoucher(address destination,bytes payload)` | one delegate-call voucher | accept | OUT-006, EGR-006 |
| `emitRawOutput(bytes output)` | the bytes **are** the output (no ABI framing) | accept | OUT-005, EGR-005, EGR-006 |
| `echo(bytes data)` | notice(data) + JSON report (sender, length, keccak) | accept | INP-001, INP-004, INP-005 |
| `batch(bytes[] calls)` | runs the calls in order (no nesting); stops at the first non-accept | that call's | FOR-027, OUT-007, OUT-008 |
| `reject(bytes report)` | emits the report, then rejects | reject | OUT-007, INP-002 |
| `raiseException(bytes payload)` | manual yield TX_EXCEPTION | `GUEST_EXCEPTION` | TRM-001, OUT-008, INS-007 |
| `halt(uint8 mode)` | 0 exit(0), 1 `reboot(RB_POWER_OFF)` (needs root, falls back to exit), 2 `abort()` | `MACHINE_HALTED` | TRM-002 |
| `manualYield(uint16 reason,uint32 data)` | raw manual yield | `UNEXPECTED_YIELD` for any reason but 1/2/4 | TRM-003 |
| `corruptOutputsRoot(uint8 mode)` | RX_ACCEPTED with 0 wrong root bytes, 1 a 31-byte payload, 2 a 33-byte payload | `INVALID_OUTPUTS_ROOT` | TRM-005, FOR-027 |
| `burnCycles(uint64 iterations,uint8 progress)` | deterministic CPU loop (~1 M iterations ≈ 10 M mcycles); progress yields when `progress` != 0 | accept | TRM-004, TRM-007, INS-004 |
| `crash(uint8 kind)` | 0 uncaught C++ exception (SIGABRT), 1 segfault, 2 `exit(3)` | the guest *process* dies, init halts the machine: `MACHINE_HALTED` (payload 134/139/3), **not** `GUEST_EXCEPTION` | TRM-001 (C++ answer) |
| `setDepositPolicy(uint8)` | 0 credit all (default), 1 reject non-token, 2 reject all | accept | FOR-026 |
| `withdrawErc20Safe(address safeErc20Transfer,address token,uint256 amount,bytes data)` | debits the ledger, emits `DelegateCallVoucher(safeErc20Transfer, safeTransfer(token, sender, amount))` (`address(0)` → `QA_SAFE_ERC20_TRANSFER`) | accept / reject on overdraft | INP-008, EGR-007, EGR-008, FOR-017 |
| libcma `WithdrawEther(uint256,bytes)`, `WithdrawErc20(address,uint256,bytes)`, `WithdrawErc721(address,uint256,bytes)`, `WithdrawErc1155Single(address,uint256,uint256,bytes)`, `TransferEther(bytes32,uint256,bytes)`, `TransferErc20(address,bytes32,uint256,bytes)`, … | ledger debit + libcma voucher (`Voucher(token.transfer)`, `safeTransferFrom`, …) | accept / reject | SMK-004, EGR-004, EGR-009, EGR-010, EGR-011 |
| `progress(uint32)` | one progress yield | accept | |
| anything else / empty | `{"error":"unknown input: …"}` or `{"error":"empty payload"}` report | reject | INP-002 |

**Deposits** (sender is a portal) are decoded with libcma and credited to the sender's wallet
account: the configured token on the accounts drive (uint96, overflow → reject), everything else
(Ether, other ERC-20s, ERC-721, ERC-1155) in the multi-asset ledger in the tail of the same
range. A JSON report echoes the decoded fields (`exec_layer_data`, `base_layer_data` in hex).

## Inspect

The payload is JSON `{"method": …, "params": […]}`:

```sh
curl -s -X POST http://localhost:6751/inspect/$APP -d '{"method":"ledger_getBalance","params":["0x<user>","'$TOKEN'"]}'
```

`ledger_getBalance [account, token?, tokenId?]` and `ledger_getTotalSupply [token?, tokenId?]`
(libcma; 32-byte big-endian report), `ledger_getNonce [address]` (the user-op nonce the engine
expects, 32-byte big-endian), `qa_fee [exponent]` (the sequencer fee `fee::to_linear(exponent)` in
token units, 32-byte big-endian), `qa_info` (config, selectors, counters), `qa_state`,
`qa_echo [hex]`, `qa_reports [count, size, pattern]`, `qa_exception [hex]` (INS-008), `qa_reject`,
`qa_burn [iterations]`. A non-JSON payload is echoed back as one report (INS-001: a full 2 MiB
query); the raw payload `state` (the sequencer's `STATE_INSPECT_QUERY`) returns progress, the
deposit policy, the pinned sequencer address and token, the nonce count, the keccak of the accounts drive and,
in the sequencer variant, `scheduler.next_batch_nonce` / `scheduler.queued_directs`.
`ledger_getBalance` for an account the ledger has never seen rejects the inspect with `Account not
found` (libcma semantics, as in the sample wallet).

## Sequencer variant

Under the scheduler every input above is a **direct input** that executes when a batch frame
covers its block (or when it is overdue), rejections and terminal commands become counted no-ops,
and signed user ops carry the same command payloads: [sequencer.md](sequencer.md).

## Terminal states: what to expect on the node

| input | emulator CLI says | node application status | input status |
|---|---|---|---|
| `raiseException` | `Manual yield tx-exception (4)`, `cmio exception with payload` | `GUEST_EXCEPTION` | `EXCEPTION` |
| `halt 0/1/2`, `crash *` | `Halted` / `Halted with payload: N`, then `machine is not yielded` | `MACHINE_HALTED` | `MACHINE_HALTED` |
| `manualYield 8 0` | `Unexpected manual yield reason 8` | `UNEXPECTED_YIELD` | `UNEXPECTED_YIELD` |
| `corruptOutputsRoot 0` | `outputs Merkle root mismatch at input N` | `INVALID_OUTPUTS_ROOT` | `INVALID_OUTPUTS_ROOT` |
| `corruptOutputsRoot 1/2` | `expected outputs Merkle root in tx buffer` | `INVALID_OUTPUTS_ROOT` | `INVALID_OUTPUTS_ROOT` |
| `reject …` | `Manual yield rx-rejected (2)` | unchanged | `REJECTED` (reports kept, alpha.13) |

Node caps per input: 65,536 outputs and 65,536 reports. The largest notice payload is 2,097,056
bytes (the ABI-encoded output is then 2,097,124 bytes, measured); one byte more cannot be emitted
(`-ENOBUFS`) and the input is rejected with `{"error":"unable to emit notice #0","code":-105}`.
