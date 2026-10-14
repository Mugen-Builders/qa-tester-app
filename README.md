# qa-tester-app

A C++ Cartesi Rollups application that can be *told*, input by input, what to do: emit N notices or
reports of a chosen size, emit vouchers and delegate-call vouchers with chosen destination, value
and payload, emit an arbitrary blob output, echo bytes, reject after reporting, raise a guest
exception, halt the machine, yield with a bogus reason, publish a wrong outputs Merkle root, burn
cycles, and keep a ledger of every portal deposit whose withdrawable balances for one ERC-20 live
in an **accounts drive** the v3 contracts can prove for emergency withdrawals. It comes in two
variants: the **classic** rollup driver, and the **sequencer** variant integrated with the
[Cartesi sequencer](https://github.com/cartesi/sequencer) `v0.1.0-alpha.10` like its canonical
app, where the same engine runs inside the machine behind the sequencer's scheduler fold and on the
host inside the sequencer binary ([docs/sequencer.md](docs/sequencer.md)).

It exists to run Track A/B of the QA cycle (catalog
[`Mugen-Builders/cartesi-qa`](https://github.com/Mugen-Builders/cartesi-qa)) against the pinned
stack: node `v2.0.0-alpha.13`, contracts `v3.0.0-alpha.10`, emulator `0.21.0`, Cartesi CLI
`2.0.0-alpha.38` (SDK `0.12.0-alpha.44`), libcma (machine-asset-tools) `v0.1.0-alpha.11`,
machine-guest-tools `0.18.0`.

It is modelled on [`machine-asset-tools/sample_apps/wallet_app`](https://github.com/Mugen-Builders/machine-asset-tools)
(libcma + libcmt) but split in two: **`libqatester`** processes inputs and knows nothing about the
rollup device; the drivers (`src/main.cpp`, `src/seq_main.cpp`) own the device. The library
implements the sequencer's
[`application-engine.h`](https://github.com/cartesi/sequencer/blob/v0.1.0-alpha.10/bindings/c-app-engine/include/application-engine.h)
C ABI (`src/lib/app_engine.cpp`): direct inputs and validated user ops in, vouchers and notices
drained out, the state in two memory ranges, `(count, clock)` progress in the state.

## Documentation

No helper scripts: every job is done with the provided tools, and the docs say how.

- [docs/build.md](docs/build.md): prerequisites, `cartesi build`/`hash`/`run`, the state NVRAM and
  the `accounts_drive_start_index` check, addresses and `QA_*` environment, CI and release artifacts.
- [docs/inputs.md](docs/inputs.md): encoding inputs with `cast`, sending them, deposits, the full
  command catalog, inspect methods, terminal states on the node.
- [docs/accounts-drive.md](docs/accounts-drive.md): ledger layout, reading and proving the accounts
  drive of a snapshot with libcma's `account-driver-reader` and the emulator.
- [docs/sequencer.md](docs/sequencer.md): the sequencer variant: what differs, the state layout,
  building, genesis for a deployment (cycle-0 image + replaced `config` range), running the
  host sequencer, user ops, tests and release assets.
- [docs/sequencer-kit.md](docs/sequencer-kit.md): the README of the devnet kit (`make seq-kit`, a
  release asset): host binaries + images + devnet state, and the anvil walkthrough.

## Layout

```
cartesi.toml               Cartesi CLI config (classic): drives root/app, [nvrams.state], machine env, [withdrawal.config]
cartesi-seq.toml           Cartesi CLI config (sequencer): [drives.app] -> app-seq target, [nvrams.config], the qa-tester-seq entrypoint
Dockerfile                 base (alpine + guest tools 0.18.0) / seq-rust (fold cross-build) / builder / app, app-seq
Makefile                   guest build, host build (libcmt mock + libcma), host-test, deps, seq-* targets
include/qatester/*.h       public API: types, sink, config, abi, commands, ledger, state, fee, engine; the vendored
                           application-engine.h and the seq_ext.h extensions
src/lib/*.cpp              the library (app_engine.cpp = the C seam)
src/main.cpp               classic rollup driver (libcmt): sink, terminal actions, raw yields
src/seq_main.cpp           sequencer guest driver: device + the scheduler fold (seq/guest) over the seam
src/genesis_main.cpp       qa-tester-genesis: the host genesis dump
seq/                       cargo workspace: guest (the fold as a C staticlib), host (qa-tester-sequencer)
tests/host/                C++ unit tests + mock-rollup end-to-end (`make host-test`)
tests/machine/             pytest driving the stored templates with the host cartesi-machine (both variants)
tests/sequencer/           pytest: anvil + host sequencer + machine replay end to end
docs/                      how to build, send inputs, prove the accounts drive and run the sequencer variant
```

## Quick start

```sh
make deps && make host-test        # host: 290 unit checks + mock e2e (needs g++ 14, cast)
cartesi build && cartesi hash      # classic template with the state NVRAMs (needs Docker + Cartesi CLI)
pytest tests/machine -v            # machine tests of the variant in .cartesi/image (cartesi-machine 0.21.0, cast)
cartesi run --epoch-length 10 --claim-staging-period 0

make seq-deps seq-host seq-test    # sequencer variant: host binary + fold tests (Rust 1.95, libclang, cmake)
make seq-image && cartesi hash     # cartesi build -c cartesi-seq.toml (replaces .cartesi/image)
make seq-cycle0 seq-kit            # cycle-0 image, then build/qa-tester-seq-devnet-linux-<arch>.tar.gz (docs/sequencer-kit.md)
pytest tests/machine/test_seq_machine.py tests/sequencer -v
```

## Design decisions

- One engine behind the sequencer's `application-engine.h`; the classic driver and the sequencer
  guest driver differ only in who decides the order (arrival vs the sequencer's scheduler fold
  compiled from `sequencer-core`). The fee port is bit-exact with `sequencer-core/src/fee.rs`
  (tested for every exponent).
- Sequencer variant: every direct input is accepted (commands included), terminal commands and
  rejections are counted no-ops, `batch` is not atomic, raw/delegate-call outputs are machine-only
  (docs/sequencer.md). The batch submitter address and the accounts-drive ERC-20 come from a 4 KiB
  `config` NVRAM read at boot and pinned in the state at genesis.
- ABI selectors for advance inputs; libcma JSON for inspect; raw `state` query reserved.
- Addresses via machine env with devnet defaults (token configurable).
- Every portal deposit is credited; only the configured ERC-20 is on the accounts drive.
- State in one NVRAM: accounts drive + single-asset ledger file-backed at its start (readable and
  provable by libcma's `account-driver-reader`), multi-asset ledger in libcma's file-mode layout
  after it (so a host dump reopens), then the meta page (progress, policy, submitter, token) and
  the nonce table; no custom accounts-drive tooling, no helper scripts.
- 2^17 accounts (machine-tool default). Both withdrawal forms (plain voucher, delegate-call).
- `crash` lands in `MACHINE_HALTED`, not `GUEST_EXCEPTION`: a C++ guest that dies takes the init
  down and the machine halts; recorded as the TRM-001 answer for native apps.
- Reject on the real machine never returns (host restores the snapshot); the driver's reject path
  reads the next request from the yield reply only so the libcmt mock e2e can continue.

## Known gaps

- Not verified in this sandbox: `cartesi run` + node JSON-RPC observations, foreclosure and
  `prove accounts-drive` end to end (needs the CLI's compose stack); the sequencer variant's full
  stack with the rollups node is a documented manual procedure (docs/sequencer.md).
- The sequencer's cockroach-recovery exporter is not drilled: a machine checkpoint's `state` and
  `ledger` ranges concatenated are the native dump format, nothing else is needed.
- `halt 1` (`reboot`) needs root; as user `dapp` it falls back to exiting (still a halt).
- ERC-721 / ERC-1155 withdrawals use libcma's vouchers (`safeTransferFrom` from the app contract);
  there is no emergency builder for them in contracts alpha.10.
- Upstream tool limits are listed in [docs/accounts-drive.md](docs/accounts-drive.md#tool-status-alpha11).
