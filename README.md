# qa-tester-app

A C++ Cartesi Rollups application that can be *told*, input by input, what to do: emit N notices or
reports of a chosen size, emit vouchers and delegate-call vouchers with chosen destination, value
and payload, emit an arbitrary blob output, echo bytes, reject after reporting, raise a guest
exception, halt the machine, yield with a bogus reason, publish a wrong outputs Merkle root, burn
cycles, and keep a ledger of every portal deposit whose withdrawable balances for one ERC-20 live
in an **accounts drive** the v3 contracts can prove for emergency withdrawals.

It exists to run Track A/B of the QA cycle (catalog
[`Mugen-Builders/cartesi-qa`](https://github.com/Mugen-Builders/cartesi-qa)) against the pinned
stack: contracts `v3.0.0-alpha.10`, **emulator `0.22.0`** (Dave `v3.0.0-alpha.6` generation; the
reference node `v2.0.0-alpha.13` still loads only `0.21` images), Cartesi CLI
`2.0.0-alpha.38` (SDK `0.12.0-alpha.44`), libcma (machine-asset-tools) `v0.1.0-alpha.11`,
machine-guest-tools `0.18.0`.

It is modelled on [`machine-asset-tools/sample_apps/wallet_app`](https://github.com/Mugen-Builders/machine-asset-tools)
(libcma + libcmt) but split in two: **`libqatester`** processes inputs and knows nothing about the
rollup device; **`src/main.cpp`** owns the device. The library API mirrors the sequencer's
[`application-engine.h`](https://github.com/cartesi/sequencer/blob/main/bindings/c-app-engine/include/application-engine.h)
concepts (direct input in, outputs streamed out, state in one file, `(count, clock)` progress) so a
C ABI shim for the sequencer is additive work.

## Documentation

No helper scripts: every job is done with the provided tools, and the docs say how.

- [docs/build.md](docs/build.md): prerequisites, `cartesi build`/`hash`/`run`, the state NVRAM and
  the `accounts_drive_start_index` check, addresses and `QA_*` environment, CI and release artifacts.
- [docs/inputs.md](docs/inputs.md): encoding inputs with `cast`, sending them, deposits, the full
  command catalog, inspect methods, terminal states on the node.
- [docs/accounts-drive.md](docs/accounts-drive.md): ledger layout, reading and proving the accounts
  drive of a snapshot with libcma's `account-driver-reader` and the emulator.

## Layout

```
cartesi.toml               Cartesi CLI config: drives root/app, [nvrams.state], machine env, [withdrawal.config]
Dockerfile                 base (alpine + guest tools 0.18.0) / builder (+ libcma dev) / app (static riscv64 binary)
Makefile                   guest build, host build (libcmt mock + libcma), host-test, deps
include/qatester/*.h       public API: types, sink, config, abi, commands, ledger, engine
src/lib/*.cpp              the library
src/main.cpp               rollup driver (libcmt): sink, terminal actions, raw yields
tests/host/                C++ unit tests + mock-rollup end-to-end (`make host-test`)
tests/machine/             pytest driving the stored template with the host cartesi-machine
docs/                      how to build, send inputs and prove the accounts drive with the stock tools
```

## Quick start

```sh
make deps && make host-test        # host: 159 unit checks + mock e2e (needs g++ 14, cast)
cartesi build && cartesi hash      # template with the state NVRAM (needs Docker + Cartesi CLI)
pytest tests/machine -v            # 24 machine tests (needs cartesi-machine 0.22.0, cast)
cartesi run --epoch-length 10 --claim-staging-period 0
```

## Design decisions

- Library shaped on application-engine.h concepts; the C ABI (`application_engine_*`), user-op
  validation, nonces and fees are **deferred**; the sequencer batch envelope is not parsed.
- ABI selectors for advance inputs; libcma JSON for inspect; raw `state` query reserved.
- Addresses via machine env with devnet defaults (token configurable).
- Every portal deposit is credited; only the configured ERC-20 is on the accounts drive.
- State in one NVRAM: accounts drive + single-asset ledger file-backed at its start (readable and
  provable by libcma's `account-driver-reader`), multi-asset ledger in buffer mode at its end; no
  custom accounts-drive tooling, no helper scripts.
- 2^17 accounts (machine-tool default). Both withdrawal forms (plain voucher, delegate-call).
- `crash` lands in `MACHINE_HALTED`, not `GUEST_EXCEPTION`: a C++ guest that dies takes the init
  down and the machine halts; recorded as the TRM-001 answer for native apps.
- Reject on the real machine never returns (host restores the snapshot); the driver's reject path
  reads the next request from the yield reply only so the libcmt mock e2e can continue.

## Known gaps

- Not verified in this sandbox: `cartesi run` + node JSON-RPC observations, foreclosure and
  `prove accounts-drive` end to end (needs the CLI's compose stack).
- `halt 1` (`reboot`) needs root; as user `dapp` it falls back to exiting (still a halt).
- ERC-721 / ERC-1155 withdrawals use libcma's vouchers (`safeTransferFrom` from the app contract);
  there is no emergency builder for them in contracts alpha.10.
- Upstream tool limits are listed in [docs/accounts-drive.md](docs/accounts-drive.md#tool-status-alpha11).
