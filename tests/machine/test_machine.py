"""Machine tests: run the stored template under the host cartesi-machine CLI, one input per
process, and check outputs byte for byte. Needs `.cartesi/image` (`cartesi build`) and Foundry's
`cast`. Run: /opt/venv/bin/pytest tests/machine -v   (or any python3 + pytest)
"""
import json
import os

import pytest

import harness as h

MAX_NOTICE = 2_097_056  # largest Notice payload that fits the 2 MiB TX buffer (OUT-002)
MAX_OUTPUT = 4 + 32 + 32 + MAX_NOTICE  # 2,097,124 bytes: selector + offset + length + payload (a 32-multiple)
TX_BUFFER = 2 * 1024 * 1024


IMAGE = os.environ.get("QA_IMAGE", str(h.ROOT / ".cartesi" / "image"))


@pytest.fixture(scope="module")
def m():
    if not os.path.isdir(IMAGE):
        pytest.skip(f"no stored machine at {IMAGE} (cartesi build)")
    if h.image_has_range(IMAGE, "config"):
        pytest.skip(f"{IMAGE} is the sequencer variant (test_seq_machine.py covers it)")
    machine = h.Machine(image=IMAGE)
    yield machine
    machine.close()


def balance(m, account, token=None):
    params = [account] + ([token] if token else [])
    q = json.dumps({"method": "ledger_getBalance", "params": params}).encode()
    r = m.inspect(q)
    assert r.returncode == 0, r.text()
    assert len(r.reports) == 1, r.reports
    return int.from_bytes(r.reports[0], "big")


# ---------------------------------------------------------------- ledger / deposits / vouchers
def test_erc20_deposit_is_credited_on_the_accounts_drive(m):
    payload = h.packed_deposit("erc20", h.TOKEN, h.USER, 1000000, bytes.fromhex("deadbeef"))
    r = m.advance(payload, sender=h.ERC20_PORTAL)
    assert r.accepted, r.text()
    rep = json.loads(r.reports[0])
    assert rep["credited"] is True and rep["accounts_drive"] is True
    assert rep["exec_layer_data"] == "0xdeadbeef" and rep["amount"] == "1000000"
    assert balance(m, h.USER, h.TOKEN) == 1_000_000


def test_other_assets_are_credited_off_the_accounts_drive(m):
    other = "0x" + "99" * 20
    r = m.advance(h.packed_deposit("erc20", other, h.USER, 77), sender=h.ERC20_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["accounts_drive"] is False
    r = m.advance(h.packed_deposit("ether", h.USER, 5), sender=h.ETHER_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["deposit"] == "ether"
    r = m.advance(h.packed_deposit("erc721", "0x" + "33" * 20, h.USER, 9, b"\x01", b"\xaa"), sender=h.ERC721_PORTAL)
    assert r.accepted and json.loads(r.reports[0])["base_layer_data"] == "0x01"
    assert balance(m, h.USER, other) == 77
    assert balance(m, h.USER) == 5


def test_plain_erc20_withdrawal_emits_a_transfer_voucher(m):
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 400000, b""))
    assert r.accepted, r.text()
    assert len(r.outputs) == 1 and r.outputs[0][:4] == h.VOUCHER_SEL
    body = r.outputs[0][4:]
    assert body[12:32].hex() == h.TOKEN[2:].lower()          # destination = token
    assert int.from_bytes(body[32:64], "big") == 0            # value
    call = h.abi_bytes_arg(body, 2)
    assert call[:4].hex() == "a9059cbb"                       # transfer(address,uint256)
    assert call[16:36].hex() == h.USER[2:].lower()
    assert int.from_bytes(call[36:68], "big") == 400000
    assert balance(m, h.USER, h.TOKEN) == 600_000


def test_safe_erc20_withdrawal_emits_a_delegate_call_voucher(m):
    safe = "0x" + "55" * 20
    r = m.advance(h.cmd("withdrawErc20Safe", safe, h.TOKEN, 100000, b""))
    assert r.accepted, r.text()
    assert r.outputs[0][:4] == h.DELEGATE_SEL
    body = r.outputs[0][4:]
    assert body[12:32].hex() == "55" * 20
    call = h.abi_bytes_arg(body, 1)
    assert call[:4].hex() == "d1660f99"                       # safeTransfer(address,address,uint256)
    assert call[16:36].hex() == h.TOKEN[2:].lower() and call[48:68].hex() == h.USER[2:].lower()
    assert int.from_bytes(call[68:100], "big") == 100000
    assert balance(m, h.USER, h.TOKEN) == 500_000


def test_overdraft_is_rejected_and_keeps_the_report(m):
    r = m.advance(h.cmd("WithdrawErc20", h.TOKEN, 500001, b""))
    assert not r.accepted and "rx-rejected" in r.text()
    rep = json.loads(r.reports[0])
    assert rep["code"] < 0 and "withdraw" in rep["error"], rep
    assert balance(m, h.USER, h.TOKEN) == 500_000


def test_accounts_drive_record_matches_LibUsdAccount(m):
    f, drive = m.read_state_drive()
    rec = f.read(32)
    f.close()
    assert drive["start"] % (1 << 22) == 0
    assert int.from_bytes(rec[:12], "little") == 500_000
    assert rec[12:32].hex() == h.USER[2:].lower()


# ---------------------------------------------------------------- outputs
def test_echo_returns_exact_bytes(m):
    data = bytes(range(256)) * 200  # 51200 bytes
    r = m.advance(h.cmd("echo", data))
    assert r.accepted and len(r.outputs) == 1
    assert h.abi_bytes_arg(r.outputs[0][4:], 0) == data
    assert json.loads(r.reports[0])["length"] == len(data)


def test_notice_at_the_2mib_limit_is_accepted_and_one_more_byte_is_rejected(m):
    r = m.advance(h.cmd("emitNotices", 1, MAX_NOTICE, 0))
    assert r.accepted, r.text()
    assert len(r.outputs) == 1 and len(r.outputs[0]) == MAX_OUTPUT
    payload = h.abi_bytes_arg(r.outputs[0][4:], 0)
    assert len(payload) == MAX_NOTICE and payload[:300] == bytes(i & 0xFF for i in range(300))
    r = m.advance(h.cmd("emitNotices", 1, MAX_NOTICE + 1, 0))
    assert not r.accepted and "rx-rejected" in r.text()
    assert json.loads(r.reports[0])["code"] == -105  # ENOBUFS
    # the machine keeps serving
    assert balance(m, h.USER, h.TOKEN) == 500_000


def test_many_reports_in_one_input(m):
    r = m.advance(h.cmd("emitReports", 20000, 16, 3), timeout=1800)
    assert r.accepted, r.text()
    assert len(r.reports) == 20000 and len(r.reports[0]) == 16


def test_raw_output_and_vouchers_and_batch(m):
    raw = bytes.fromhex("0102030405")
    r = m.advance(h.cmd("emitRawOutput", raw))
    assert r.accepted and r.outputs == [raw]
    dest = "0x" + "42" * 20
    r = m.advance(h.cmd("emitVouchers", 2, dest, 7, b"\xab"))
    assert r.accepted and len(r.outputs) == 2 and all(o[:4] == h.VOUCHER_SEL for o in r.outputs)
    calls = [h.cmd("emitReport", b"\x01"), h.cmd("emitNotice", b"\x02"), h.cmd("emitDelegateCallVoucher", dest, b"\x03")]
    r = m.advance(h.cmd("batch", calls))
    assert r.accepted and len(r.outputs) == 2 and r.reports == [b"\x01"]
    assert r.outputs[0][:4] == h.NOTICE_SEL and r.outputs[1][:4] == h.DELEGATE_SEL


def test_reject_keeps_report_and_drops_outputs(m):
    calls = [h.cmd("emitNotice", b"\x02"), h.cmd("reject", b"AB")]
    r = m.advance(h.cmd("batch", calls))
    assert not r.accepted and "rx-rejected" in r.text()
    assert r.reports == [b"AB"] and r.outputs == [] and len(r.rejected_outputs) == 1


def test_unknown_and_empty_inputs_are_rejected_with_a_report(m):
    r = m.advance(bytes.fromhex("deadbeef00"))
    assert not r.accepted and b"unknown input" in r.reports[0]
    r = m.advance(b"")
    assert not r.accepted and b"empty payload" in r.reports[0]


# ---------------------------------------------------------------- inspect
def test_inspect_state_and_echo_and_exception(m):
    r = m.inspect(b"state")
    assert r.returncode == 0 and json.loads(r.reports[0])["executed_input_count"] >= 10
    big = bytes(range(256)) * (TX_BUFFER // 256)
    r = m.inspect(big)
    assert r.returncode == 0 and r.reports == [big]
    r = m.inspect(json.dumps({"method": "qa_reports", "params": [3, 8, 2]}).encode())
    assert r.returncode == 0 and r.reports == [b"\xff" * 8] * 3
    r = m.inspect(json.dumps({"method": "qa_exception", "params": ["0x0102"]}).encode())
    assert "tx-exception" in r.text()
    r = m.inspect(json.dumps({"method": "qa_info"}).encode())
    info = json.loads(r.reports[0])
    assert info["selectors"]["emitNotices"]["selector"] == "0x3fd67492"


# ---------------------------------------------------------------- terminal outcomes (fresh template each)
@pytest.mark.parametrize("command,marker", [
    (("raiseException", b"oops"), "tx-exception"),
    (("halt", 0), "Halted"),
    (("halt", 2), "Halted with payload: 134"),
    (("manualYield", 8, 0), "Unexpected manual yield reason 8"),
    (("crash", 0), "Halted with payload: 134"),
    (("crash", 1), "Halted with payload: 139"),
    (("corruptOutputsRoot", 0), "outputs Merkle root mismatch"),
    (("corruptOutputsRoot", 1), "expected outputs Merkle root in tx buffer"),
    (("corruptOutputsRoot", 2), "expected outputs Merkle root in tx buffer"),
])
def test_terminal_outcomes(command, marker):
    fresh = h.Machine(image=IMAGE)
    try:
        r = fresh.advance(h.cmd(*command), commit=False)
        assert not r.accepted
        assert marker in r.text(), r.text()[-800:]
    finally:
        fresh.close()


def test_corrupt_root_after_outputs_keeps_the_outputs_visible():
    fresh = h.Machine(image=IMAGE)
    try:
        calls = [h.cmd("emitVouchers", 1, "0x" + "42" * 20, 0, b""), h.cmd("emitNotice", b"\x02"),
                 h.cmd("corruptOutputsRoot", 0)]
        r = fresh.advance(h.cmd("batch", calls), commit=False)
        assert "outputs Merkle root mismatch" in r.text()
        assert "tx-output" in r.text() and r.text().count("tx-output") == 2
    finally:
        fresh.close()


def test_burn_cycles_grows_mcycle():
    fresh = h.Machine(image=IMAGE)
    try:
        r0 = fresh.advance(h.cmd("burnCycles", 1000, 0), commit=False)
        r1 = fresh.advance(h.cmd("burnCycles", 20_000_000, 1), commit=False)
        assert r0.accepted and r1.accepted
        def cycles(r):
            return [int(l.split()[-1]) for l in r.text().splitlines() if l.startswith("Cycles:")][-1]
        assert cycles(r1) - cycles(r0) > 20_000_000
        assert "progress" in r1.text().lower()
    finally:
        fresh.close()
