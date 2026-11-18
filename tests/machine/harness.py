"""Drive a stored qa-tester template with the host `cartesi-machine` CLI, one input per run,
the same way the node's advancer does (an on-disk clone of the image, advance with a stored
snapshot, keep it when accepted), so every test is byte-exact and needs nothing but the emulator
(0.22.0), the image under .cartesi/image and Foundry's `cast` (the same encoder the docs tell
humans to use: docs/inputs.md).
"""
import glob
import json
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

APP = "0x" + "ab" * 20
USER = "0x1111111111111111111111111111111111111111"
# sequencer variant: batch submitter = anvil account 9 (the devnet default written by `make seq/config-devnet.bin`)
SEQUENCER = "0xa0Ee7A142d267C1f36714E4a8F75612F20a79720"
SEQUENCER_KEY = "0x2a871d0798f97d79848a013d4936a73bf4cc922c825d33c1cf7073dff6d409c6"
# a signing user: anvil account 1
SIGNER = "0x70997970C51812dc3A010C7d01b50e0d17dc79C8"
SIGNER_KEY = "0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d"
MAX_WAIT_BLOCKS = 1200
TOKEN = "0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81"
ERC20_PORTAL = "0x3332DE61a8BB9aC84893b2f552Fe81C9a6dC5419"
ETHER_PORTAL = "0x035b11Be55656c6cfC822D1CaE568C1Af2e497b0"
ERC721_PORTAL = "0x397c352d18DFf47CC8a6143403142cf7afd5Ff7E"

VOUCHER_SEL = bytes.fromhex("237a816f")
NOTICE_SEL = bytes.fromhex("c258d6e5")
DELEGATE_SEL = bytes.fromhex("10321e8b")

# Advance-input commands used by the tests (full catalog: docs/inputs.md). `cast` derives the
# selectors; the host unit tests cross-check them against the C++ table.
SIGNATURES = {
    "emitNotices": "emitNotices(uint32,uint32,uint8)",
    "emitNotice": "emitNotice(bytes)",
    "emitReports": "emitReports(uint32,uint32,uint8)",
    "emitReport": "emitReport(bytes)",
    "emitVouchers": "emitVouchers(uint32,address,uint256,bytes)",
    "emitDelegateCallVoucher": "emitDelegateCallVoucher(address,bytes)",
    "emitRawOutput": "emitRawOutput(bytes)",
    "echo": "echo(bytes)",
    "batch": "batch(bytes[])",
    "reject": "reject(bytes)",
    "raiseException": "raiseException(bytes)",
    "halt": "halt(uint8)",
    "manualYield": "manualYield(uint16,uint32)",
    "corruptOutputsRoot": "corruptOutputsRoot(uint8)",
    "burnCycles": "burnCycles(uint64,uint8)",
    "crash": "crash(uint8)",
    "setDepositPolicy": "setDepositPolicy(uint8)",
    "withdrawErc20Safe": "withdrawErc20Safe(address,address,uint256,bytes)",
    "progress": "progress(uint32)",
    "WithdrawEther": "WithdrawEther(uint256,bytes)",
    "WithdrawErc20": "WithdrawErc20(address,uint256,bytes)",
}
EVM_ADVANCE = "EvmAdvance(uint256,address,address,uint256,uint256,uint256,uint256,bytes)"


def _hex(v) -> str:
    """cast argument: bytes -> 0x-hex, lists -> [a,b,...], everything else as text."""
    if isinstance(v, (bytes, bytearray)):
        return "0x" + bytes(v).hex()
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(_hex(x) for x in v) + "]"
    return str(v)


def cast(*args) -> bytes:
    p = subprocess.run(["cast", *args], capture_output=True, text=True, check=True)
    return bytes.fromhex(p.stdout.strip()[2:])


def cmd(name, *args) -> bytes:
    """ABI calldata for one command, exactly `cast calldata '<signature>' <args>`."""
    return cast("calldata", SIGNATURES[name], *(_hex(a) for a in args))


def evm_advance(payload: bytes, sender: str, app: str = APP, block: int = 1, index: int = 0,
                chain: int = 31337, timestamp: int = 1700000000) -> bytes:
    """The EvmAdvance(...) input the InputBox would emit (prevRandao = 0)."""
    return cast("calldata", EVM_ADVANCE, str(chain), app, sender, str(block), str(timestamp), "0",
                str(index), _hex(payload))


def packed_deposit(kind: str, *args) -> bytes:
    """Portal payloads (abi.encodePacked), see docs/inputs.md."""
    if kind == "erc20":  # token, sender, amount, execLayerData
        token, sender, amount, exec_ = (list(args) + [b""])[:4]
        return cast("abi-encode", "--packed", "(address,address,uint256,bytes)", token, sender, str(amount), _hex(exec_))
    if kind == "ether":  # sender, amount, execLayerData
        sender, amount, exec_ = (list(args) + [b""])[:3]
        return cast("abi-encode", "--packed", "(address,uint256,bytes)", sender, str(amount), _hex(exec_))
    if kind == "erc721":  # token, sender, tokenId, abi(bytes baseLayerData, bytes execLayerData)
        token, sender, token_id, base, exec_ = (list(args) + [b"", b""])[:5]
        tail = cast("abi-encode", "f(bytes,bytes)", _hex(base), _hex(exec_))
        return cast("abi-encode", "--packed", "(address,address,uint256,bytes)", token, sender, str(token_id), _hex(tail))
    raise ValueError(f"unknown deposit kind {kind}")


# ---------------------------------------------------------------- sequencer batches (SSZ)
# sequencer-core/src/batch.rs: Batch{nonce u64, frames List<Frame>}, Frame{user_ops List<WireUserOp>,
# safe_block u64, fee_price u16}, WireUserOp{nonce u32, max_fee u16, data List<u8>, signature List<u8>}.
# SSZ containers: fixed fields in place, variable ones as 4-byte offsets followed by their bytes.
# seq/guest/tests/fold.rs pins the same vector (ssz_vector_for_the_python_harness).
def _ssz_container(fixed_parts, variable_parts):
    """fixed_parts: list of bytes or None (None = a variable field at that position, in order)."""
    fixed_len = sum(len(p) if p is not None else 4 for p in fixed_parts)
    out = b""
    offset = fixed_len
    vi = 0
    for p in fixed_parts:
        if p is None:
            out += offset.to_bytes(4, "little")
            offset += len(variable_parts[vi])
            vi += 1
        else:
            out += p
    return out + b"".join(variable_parts)


def _ssz_list_of_variable(items: list) -> bytes:
    offsets = b""
    off = 4 * len(items)
    for it in items:
        offsets += off.to_bytes(4, "little")
        off += len(it)
    return offsets + b"".join(items)


def user_op(nonce: int, max_fee: int, data: bytes, signature: bytes) -> bytes:
    return _ssz_container([nonce.to_bytes(4, "little"), max_fee.to_bytes(2, "little"), None, None], [data, signature])


def frame(safe_block: int, fee_price: int = 0, user_ops=()) -> bytes:
    return _ssz_container([None, safe_block.to_bytes(8, "little"), fee_price.to_bytes(2, "little")],
                          [_ssz_list_of_variable(list(user_ops))])


def batch(nonce: int, frames) -> bytes:
    """SSZ Batch; `frames` are `frame(...)` byte strings."""
    return _ssz_container([nonce.to_bytes(8, "little"), None], [_ssz_list_of_variable(list(frames))])


def sign_user_op(private_key: str, nonce: int, max_fee: int, data: bytes, chain: int = 31337, app: str = APP) -> bytes:
    """EIP-712 signature of UserOp{uint32 nonce; uint16 max_fee; bytes data} under the sequencer's
    domain (CartesiAppSequencer/1, verifyingContract = app), produced by `cast wallet sign --data`.
    The 65 bytes (r, s, v) are what the sequencer's POST /tx and the batch envelope carry."""
    typed = {
        "types": {
            "EIP712Domain": [{"name": "name", "type": "string"}, {"name": "version", "type": "string"},
                             {"name": "chainId", "type": "uint256"}, {"name": "verifyingContract", "type": "address"}],
            "UserOp": [{"name": "nonce", "type": "uint32"}, {"name": "max_fee", "type": "uint16"},
                       {"name": "data", "type": "bytes"}],
        },
        "primaryType": "UserOp",
        "domain": {"name": "CartesiAppSequencer", "version": "1", "chainId": chain, "verifyingContract": app},
        "message": {"nonce": nonce, "max_fee": max_fee, "data": "0x" + data.hex()},
    }
    p = subprocess.run(["cast", "wallet", "sign", "--private-key", private_key, "--data", json.dumps(typed)],
                       capture_output=True, text=True, check=True)
    sig = bytes.fromhex(p.stdout.strip()[2:])
    assert len(sig) == 65, sig.hex()
    return sig


def checksum(address: str) -> str:
    """EIP-55 casing (what the sequencer's JSON carries)."""
    p = subprocess.run(["cast", "to-check-sum-address", address], capture_output=True, text=True, check=True)
    return p.stdout.strip()


def image_config(image) -> dict:
    cfg = json.load(open(os.path.join(image, "config.json")))
    return cfg.get("config", cfg)


def image_has_range(image, label: str) -> bool:
    cfg = image_config(image)
    return any(d.get("label") == label for d in cfg.get("nvram", []) + cfg.get("flash_drive", []))


@dataclass
class Result:
    returncode: int
    stdout: str
    stderr: str
    outputs: list = field(default_factory=list)          # accepted outputs (raw bytes)
    rejected_outputs: list = field(default_factory=list)
    reports: list = field(default_factory=list)
    outputs_root: bytes | None = None
    final_hash: str | None = None

    @property
    def accepted(self):
        return self.returncode == 0 and self.outputs_root is not None

    def notices(self):
        return [o[4:] for o in self.outputs if o[:4] == NOTICE_SEL]

    def text(self):
        return self.stdout + self.stderr


def abi_bytes_arg(data: bytes, index: int = 0) -> bytes:
    """Decode a single dynamic `bytes` argument at head position `index` of an ABI tuple."""
    off = int.from_bytes(data[32 * index:32 * index + 32], "big")
    n = int.from_bytes(data[off:off + 32], "big")
    return data[off + 32:off + 32 + n]


class Machine:
    def __init__(self, image=ROOT / ".cartesi" / "image", keep=False):
        self.tmp = tempfile.mkdtemp(prefix="qa-tester-machine-")
        self.image = os.path.join(self.tmp, "image")
        shutil.copytree(image, self.image)
        self.index = 0
        self.block = 100
        self.last_proof = None  # outputs-merkle-root proof of the previous accepted input
        self.last_root = None   # outputs Merkle root file of the last accepted input (None: empty tree)
        self.keep = keep
        self.runs = 0

    def close(self):
        if not self.keep:
            shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, args, timeout=600, load=None):
        cmd = ["cartesi-machine", f"--load={load or self.image}"] + args
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p

    def advance(self, payload: bytes, sender=USER, commit=True, check_root=True, timeout=600,
                max_mcycle=None, block=None, raw=None):
        """One input. `block` overrides the running block number (the sequencer tests choose the
        inclusion blocks that frames cover); the next input continues after it. `raw` feeds a
        ready-made EvmAdvance input (the bytes an InputBox InputAdded event carries) instead."""
        work = os.path.join(self.tmp, f"work-{self.runs}")
        self.runs += 1
        os.makedirs(work)
        machine = os.path.join(work, "machine")
        if block is not None:
            self.block = block
        if raw is None:
            raw = evm_advance(payload, sender, app=APP, block=self.block, index=self.index)
        with open(os.path.join(work, f"input-{self.index}.bin"), "wb") as f:
            f.write(raw)
        fields = [
            f"input:{work}/input-%i.bin",
            f"output:{work}/input-%i-output-%o.bin",
            f"rejected_output:{work}/input-%i-rejected-%o.bin",
            f"report:{work}/input-%i-report-%o.bin",
            f"outputs_merkle_root:{work}/input-%i-outputs-merkle-root.bin",
            f"outputs_merkle_root_proof:{work}/input-%i-outputs-merkle-root-proof.json",
            f"output_proof:{work}/output-%o-input-%i-proof.json",
            "format:json",
            f"input_file_index_begin:{self.index}",
            f"input_file_index_end:{self.index + 1}",
            f"check_outputs_merkle_root:{'true' if check_root else 'false'}",
        ]
        if self.last_proof is not None and check_root:
            fields.append(f"last_output_proof:{self.last_proof}")
        # emulator 0.22: the run works on an on-disk clone of the image (sharing:all) and
        # --revert-mode=stored snapshots it, so an accepted input leaves the new state in the clone
        # and a rejected one leaves the state the input started from (no --store)
        root = f"{work}/input-{self.index}-outputs-merkle-root.bin"
        args = ["--revert-mode=stored", "--final-hash", "--cmio-advance-state=" + ",".join(fields)]
        if max_mcycle is not None:
            args.append(f"--max-mcycle={max_mcycle}")
        p = self._run(args, timeout=timeout, load=f"{machine},clone:{self.image}")
        res = Result(p.returncode, p.stdout, p.stderr)
        last_number = lambda x: int(x.rsplit("-", 1)[1].split(".")[0])  # %o: global for outputs, per input for reports
        for f in sorted(glob.glob(f"{work}/input-{self.index}-output-*.bin"), key=last_number):
            res.outputs.append(open(f, "rb").read())
        for f in sorted(glob.glob(f"{work}/input-{self.index}-rejected-*.bin"), key=last_number):
            res.rejected_outputs.append(open(f, "rb").read())
        for f in sorted(glob.glob(f"{work}/input-{self.index}-report-*.bin"), key=last_number):
            res.reports.append(open(f, "rb").read())
        if os.path.exists(root):
            res.outputs_root = open(root, "rb").read()
        for line in p.stdout.splitlines():  # --final-hash prints "<mcycle>: 0x<hash>"
            if line.split(": 0x")[0].isdigit() and len(line.split(": 0x")[-1]) == 64:
                res.final_hash = line.split(": ")[-1].strip()
        # the CLI resumes the outputs Merkle tree from the proof of the last *output* of the last run
        proofs = sorted(glob.glob(f"{work}/output-*-input-{self.index}-proof.json"),
                        key=lambda x: int(os.path.basename(x).split("-")[1]))
        if commit and res.accepted and os.path.isdir(machine):
            shutil.rmtree(self.image)
            os.rename(machine, self.image)
            if proofs:
                self.last_proof = proofs[-1]
            self.last_root = root
            self.index += 1
            self.block += 1
        else:
            shutil.rmtree(machine, ignore_errors=True)
            shutil.rmtree(machine + ".revert", ignore_errors=True)
        return res

    def inspect(self, payload: bytes, timeout=600):
        work = os.path.join(self.tmp, f"inspect-{self.runs}")
        self.runs += 1
        os.makedirs(work)
        with open(os.path.join(work, "query.bin"), "wb") as f:
            f.write(payload)
        # emulator 0.22: the rolling-template assertion compares the tx buffer with an expected
        # outputs Merkle root, the empty tree unless a file names the root of the last accepted input
        template = "--assert-rolling-template" + (f"=outputs_merkle_root:{self.last_root}" if self.last_root else "")
        p = self._run(["--revert-mode=none", template,
                       "--cmio-inspect-state=" + f"query:{work}/query.bin,report:{work}/query-report-%o.bin"],
                      timeout=timeout)
        res = Result(p.returncode, p.stdout, p.stderr)
        for f in sorted(glob.glob(f"{work}/query-report-*.bin"),
                        key=lambda x: int(x.rsplit("-", 1)[1].split(".")[0])):
            res.reports.append(open(f, "rb").read())
        return res

    def read_range(self, label="state", image=None):
        """The stored file of a labeled range plus its config entry."""
        cfg = image_config(image or self.image)
        for d in cfg.get("nvram", []) + cfg.get("flash_drive", []):
            if d.get("label") == label:
                name = os.path.basename(d["backing_store"]["data_filename"])
                return open(os.path.join(image or self.image, name), "rb"), d
        raise FileNotFoundError(f"range {label} not found")

    def read_state_drive(self, image=None):
        return self.read_range("state", image)

    def state_bytes(self, image=None) -> bytes:
        """The comparison bytes of the application: the whole `state` range, exactly the host
        engine's dump (docs/sequencer.md)."""
        f, _ = self.read_range("state", image)
        try:
            return f.read()
        finally:
            f.close()

