"""Drive a stored qa-tester template with the host `cartesi-machine` CLI, one input per run,
the same way the node's advancer does (fresh load, advance, store), so every test is byte-exact and
needs nothing but the emulator, the kernel, the image under .cartesi/image and Foundry's `cast`
(the same encoder the docs tell humans to use: docs/inputs.md).
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
        self.keep = keep
        self.runs = 0

    def close(self):
        if not self.keep:
            shutil.rmtree(self.tmp, ignore_errors=True)

    def _run(self, args, timeout=600):
        cmd = ["cartesi-machine", f"--load={self.image}"] + args
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p

    def advance(self, payload: bytes, sender=USER, commit=True, check_root=True, timeout=600,
                max_mcycle=None):
        work = os.path.join(self.tmp, f"work-{self.runs}")
        self.runs += 1
        os.makedirs(work)
        new_image = os.path.join(work, "new_image")
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
            f"input_index_begin:{self.index}",
            f"input_index_end:{self.index + 1}",
            f"check_outputs_merkle_root:{'true' if check_root else 'false'}",
        ]
        if self.last_proof is not None and check_root:
            fields.append(f"last_output_proof:{self.last_proof}")
        args = [f"--store={new_image}", "--revert-mode=none", "--assert-rolling-template", "--final-hash",
                "--cmio-advance-state=" + ",".join(fields)]
        if max_mcycle is not None:
            args.append(f"--max-mcycle={max_mcycle}")
        p = self._run(args, timeout=timeout)
        res = Result(p.returncode, p.stdout, p.stderr)
        for f in sorted(glob.glob(f"{work}/input-{self.index}-output-*.bin")):
            res.outputs.append(open(f, "rb").read())
        for f in sorted(glob.glob(f"{work}/input-{self.index}-rejected-*.bin")):
            res.rejected_outputs.append(open(f, "rb").read())
        for f in sorted(glob.glob(f"{work}/input-{self.index}-report-*.bin"),
                        key=lambda x: int(x.rsplit("-", 1)[1].split(".")[0])):
            res.reports.append(open(f, "rb").read())
        root = f"{work}/input-{self.index}-outputs-merkle-root.bin"
        if os.path.exists(root):
            res.outputs_root = open(root, "rb").read()
        for line in p.stdout.splitlines():
            if "Final hash" in line or line.startswith("final hash"):
                res.final_hash = line.split(":")[-1].strip()
        # the CLI resumes the outputs Merkle tree from the proof of the last *output* of the last run
        proofs = sorted(glob.glob(f"{work}/output-*-input-{self.index}-proof.json"),
                        key=lambda x: int(os.path.basename(x).split("-")[1]))
        if commit and res.accepted and os.path.isdir(new_image):
            shutil.rmtree(self.image)
            shutil.copytree(new_image, self.image)
            if proofs:
                self.last_proof = proofs[-1]
            self.index += 1
            self.block += 1
        return res

    def inspect(self, payload: bytes, timeout=600):
        work = os.path.join(self.tmp, f"inspect-{self.runs}")
        self.runs += 1
        os.makedirs(work)
        with open(os.path.join(work, "query.bin"), "wb") as f:
            f.write(payload)
        p = self._run(["--revert-mode=none", "--assert-rolling-template",
                       "--cmio-inspect-state=" + f"query:{work}/query.bin,report:{work}/query-report-%o.bin"],
                      timeout=timeout)
        res = Result(p.returncode, p.stdout, p.stderr)
        for f in sorted(glob.glob(f"{work}/query-report-*.bin"),
                        key=lambda x: int(x.rsplit("-", 1)[1].split(".")[0])):
            res.reports.append(open(f, "rb").read())
        return res

    def read_state_drive(self, image=None, length=64 * 1024 * 1024):
        cfg = json.load(open(os.path.join(image or self.image, "config.json")))
        cfg = cfg.get("config", cfg)
        for d in cfg.get("nvram", []) + cfg.get("flash_drive", []):
            if d.get("label") == "state" or d.get("length") == length:
                name = os.path.basename(d["backing_store"]["data_filename"])
                return open(os.path.join(image or self.image, name), "rb"), d
        raise FileNotFoundError("state range not found")

