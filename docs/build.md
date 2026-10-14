# Build, run and configure

Everything here uses the Cartesi CLI, `cartesi-machine` and `jq`. There are no helper scripts.

## Prerequisites

| tool | version | used for |
|---|---|---|
| Docker with `linux/riscv64` emulation (`docker/setup-qemu-action` on CI, `binfmt` locally) | any recent | `cartesi build` builds the drives from the `Dockerfile` |
| Cartesi CLI | `2.0.0-alpha.38` (`npm i -g @cartesi/cli@2.0.0-alpha.38`) | build, hash, run, send, deposit, address-book |
| `cartesi-machine` + kernel | emulator `0.21.0`, `linux-6.5.13-ctsi-2-v0.21.0` (the SDK `0.12.0-alpha.44` pair) | only for `tests/machine` and manual snapshot work; the CLI boots through the SDK image otherwise |
| Foundry `cast` | `1.4.x` | encoding inputs ([inputs.md](inputs.md)) and the tests |
| `jq` | any | the accounts-drive index check below |

## Build the template

```sh
cartesi build            # root.ext2 + app.sqfs from the Dockerfile, then boots and stores .cartesi/image
cartesi hash             # template hash
```

`cartesi.toml` declares the state as one 64 MiB NVRAM (`[nvrams.state]`, `/dev/uio0`: a UIO
range with no page cache and no filesystem), so a plain `cartesi build` produces a template that
already contains the state range. The emulator places NVRAMs after the last flash drive, each
aligned to its own length: with the current root and app drives `state` lands at `0x8c000000`
(the sequencer variant adds the 4 KiB `config` range at `0x90000000`, [sequencer.md](sequencer.md)).

Template hashes with the pinned stack (`cartesi build`, CLI 2.0.0-alpha.38, libcma alpha.11,
state layout version 1 with the meta page and nonce table): see the end of this page. The CLI
quotes the `[machine.env]` exports in the machine's init script, so a hand-booted machine hashes
differently; `cartesi build` is the reference.

### Check `accounts_drive_start_index` after every build

`[withdrawal.config].accounts_drive_start_index` must equal the state NVRAM start divided by
`2^(5 + log2_max_num_of_accounts + log2_leaves_per_account)` = `2^22`. It depends on the drive
sizes, so re-check it whenever the root or app drive changes size class:

```sh
start=$(jq -r '(.config // .).nvram[] | select(.label == "state") | .start' .cartesi/image/config.json)
echo $((start >> 22))                     # must match accounts_drive_start_index in cartesi.toml (560)
```

CI runs this same check and fails when the values differ.

## Run on the devnet

```sh
cartesi run --epoch-length 10 --claim-staging-period 0      # anvil + node (Docker compose)
```

`cartesi run`/`cartesi deploy` pass `[withdrawal.config]` to `cartesi-rollups-cli deploy application
--withdrawal-config` (guardian = anvil account 0, `2^17` accounts of one 32-byte leaf,
`TestUsdWithdrawalOutputBuilder`). For another chain edit the `[machine.env]` addresses, the builder
and the guardian.

## Addresses

Devnet defaults are the `cartesi address-book` of rollups-contracts `v3.0.0-alpha.10`. The ledger
token defaults to **TestUsdc** `0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81` (what the USD withdrawal
builder pays out). The CLI devnet also ships TestToken; check the running devnet's address book
before depositing. `QA_SAFE_ERC20_TRANSFER` is not in the address book: read it from the builder,

```sh
cast call $BUILDER 'buildWithdrawalOutput(address,bytes)(bytes)' $APP 0x<32-byte account>
```

and decode the delegate-call voucher's destination, or pass it explicitly in the input.

| env (`[machine.env]`) | default | meaning |
|---|---|---|
| `QA_TOKEN` | TestUsdc | the ERC-20 whose balances are the accounts drive; pinned in the state at genesis (the sequencer variant takes it from the `config` range) |
| `QA_ETHER_PORTAL`, `QA_ERC20_PORTAL`, `QA_ERC721_PORTAL`, `QA_ERC1155_SINGLE_PORTAL`, `QA_ERC1155_BATCH_PORTAL` | alpha.10 devnet | deposit senders |
| `QA_SAFE_ERC20_TRANSFER` | 0 | default `withdrawErc20Safe` destination |
| `QA_DEPOSIT_POLICY` | 0 | 0 credit all, 1 reject non-token deposits, 2 reject all |
| `QA_LOG` | 1 | stderr logging |
| `QA_STATE_DEVICE` | `/dev/uio0` (argv[1] wins) | the state range: accounts drive, both ledgers, meta page, nonce table |
| `QA_SINGLE_ACCOUNTS`, `QA_REGION_A_LENGTH`, `QA_REGION_B_LENGTH`, `QA_REGION_C_LENGTH`, `QA_MULTI_ACCOUNTS`, `QA_MULTI_ASSETS`, `QA_MULTI_BALANCES` | 2^17, 32 MiB, 28 MiB, 4 MiB, 8192, 64, 65536 | state layout (changing them changes the accounts drive and the dump size) |
| `QA_ENGINE_WORKDIR` | `$TMPDIR` or `/tmp` | host only: where the sequencer's engine keeps its private copy of a dump |

## Host build and tests

```sh
make deps            # clones machine-guest-tools v0.18.0 + machine-asset-tools v0.1.0-alpha.11, builds libcmt mock + libcma
make host-test       # unit tests (selectors vs cast, ABI, ledger routing, engine flows, meta page, nonces, fees, the C seam) + mock-rollup e2e (needs cast)
pytest tests/machine -v     # drives .cartesi/image with the host cartesi-machine, one input per process (needs cast)
```

The sequencer variant's host binary, fold tests, image, cycle-0 image and end-to-end test are in
[sequencer.md](sequencer.md) (`make seq-deps seq-host seq-test seq-image seq-cycle0`).

## Release artifacts (CI)

`.github/workflows/build-and-release.yml`: `host-tests` runs `make deps seq-deps`, `make host-test`
and `make seq-host seq-test seq-guest-lib` with g++-14 and Rust 1.95.0, and packages
`qa-tester-sequencer-linux-amd64.tar.gz` (the host sequencer + `qa-tester-genesis`);
`build-and-test` installs emulator `0.21.0`, the matching kernel, xgenext2fs/mksquashfs, Foundry,
Rust and the Cartesi CLI, runs `cartesi build`, `cartesi hash`, the index check above and the
classic machine tests, archives `qa-tester-snapshot.tar.gz` + `qa-tester-app.sqfs`, then builds the
sequencer variant (`make seq-image`, the range checks, `make seq-cycle0` and the hash-equality
check, its machine tests and the end-to-end test) and archives `qa-tester-seq-genesis-devnet.tar.gz`,
`qa-tester-seq-cycle0.tar.gz`, `qa-tester-seq-app.sqfs` and the devnet kit
`qa-tester-seq-devnet-linux-x86_64.tar.gz` (`make seq-kit`, [sequencer-kit.md](sequencer-kit.md));
`release` attaches everything (with sha256 files) to a GitHub pre-release on `v*` tags; the
workflow also runs on `workflow_dispatch`, with the artifacts kept for 30 days. Versions are repository variables
(`EMULATOR_VERSION`, `MACHINE_LINUX_VERSION`, `KERNEL_VERSION`, `XGENEXT2_VERSION`,
`CARTESI_CLI_VERSION`, `FOUNDRY_VERSION`, `RUST_VERSION`).

## Template hashes

| variant | built | hash |
|---|---|---|
| classic (`cartesi build`) | 2026-10-13 | `0x40e49f3d83dbb2ee78ee7d8bdb33a1448b1ac7a1f7ea2eb926e5047ecb3de4e7` |
| sequencer (`cartesi build -c cartesi-seq.toml`, devnet submitter + TestUsdc in the `config` range; the cycle-0 image boots to the same hash) | 2026-10-13 | `0x697b5384b7598e8fcbcc3c208a1855b534cabcaed1e55fff689c60dc5dcb8997` |
