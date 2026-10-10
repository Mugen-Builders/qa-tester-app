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

`cartesi.toml` declares the ledger state as one 64 MiB NVRAM (`[nvrams.state]`, `/dev/uio0`, a UIO
range with no page cache and no filesystem), so a plain `cartesi build` produces a template that
already contains the state range. The emulator places the NVRAM after the last flash drive, aligned
to its own length: with the current root and app drives it lands at `0x8c000000`.

Template hash with the pinned stack (2026-10-09, `cartesi build` with CLI 2.0.0-alpha.38, libcma
alpha.11): `0x8e4f27b67edb41b0da389e5de1e6506b19be693bae28a61e812a104834be8c68`
(`app.sqfs` sha256 `c0d022a7…92eac`, byte-identical to the libcma alpha.10 build). The hash of the
earlier hand-booted template differed only because the CLI quotes the `[machine.env]` exports in the
machine's init script; `cartesi build` is the reference from now on.

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
| `QA_TOKEN` | TestUsdc | the ERC-20 whose balances are the accounts drive |
| `QA_ETHER_PORTAL`, `QA_ERC20_PORTAL`, `QA_ERC721_PORTAL`, `QA_ERC1155_SINGLE_PORTAL`, `QA_ERC1155_BATCH_PORTAL` | alpha.10 devnet | deposit senders |
| `QA_SAFE_ERC20_TRANSFER` | 0 | default `withdrawErc20Safe` destination |
| `QA_DEPOSIT_POLICY` | 0 | 0 credit all, 1 reject non-token deposits, 2 reject all |
| `QA_LOG` | 1 | stderr logging |
| `QA_STATE_DEVICE` | `/dev/uio0` (argv[1] wins) | state range: accounts drive, single-asset ledger, multi-asset ledger |
| `QA_LEDGER_DEVICE` | unset (argv[2] wins) | optional separate range for the multi-asset ledger |
| `QA_SINGLE_ACCOUNTS`, `QA_REGION_A_LENGTH`, `QA_REGION_B_LENGTH`, `QA_MULTI_ACCOUNTS`, `QA_MULTI_ASSETS`, `QA_MULTI_BALANCES` | 2^17, 32 MiB, 32 MiB, 8192, 64, 65536 | ledger layout (changing them changes the accounts drive) |

## Host build and tests

```sh
make deps            # clones machine-guest-tools v0.18.0 + machine-asset-tools v0.1.0-alpha.11, builds libcmt mock + libcma
make host-test       # unit tests (selectors vs cast, ABI, ledger routing, engine flows) + mock-rollup e2e (needs cast)
pytest tests/machine -v     # drives .cartesi/image with the host cartesi-machine, one input per process (needs cast)
```

## Release artifacts (CI)

`.github/workflows/build-and-release.yml`: `host-tests` runs `make deps` + `make host-test` with
g++-14; `build-and-test` installs emulator `0.21.0`, the matching kernel, xgenext2fs/mksquashfs,
Foundry and the Cartesi CLI, runs `cartesi build`, `cartesi hash`, the index check above and the
machine tests, then uploads `qa-tester-snapshot.tar.gz` (the stored template) and
`qa-tester-app.sqfs` with sha256 files; `release` attaches them to a GitHub pre-release on `v*`
tags. Versions are repository variables (`EMULATOR_VERSION`, `MACHINE_LINUX_VERSION`,
`KERNEL_VERSION`, `XGENEXT2_VERSION`, `CARTESI_CLI_VERSION`, `FOUNDRY_VERSION`).
