#!/usr/bin/env python3
"""Exercise restart admission through the v3 bridge and a real QEMU click."""
import argparse
import json
import re
from pathlib import Path
import subprocess
import time
import uuid

import test_usb_system_qemu as fixture


def power(guest, operation, request, **extra):
    return guest.call("hw_power", request_id=request, operation=operation, **extra)


def click_not_now(guest):
    qmp = fixture.Qmp(guest.sock)
    qmp.call("device_add", driver="usb-mouse", bus="usb.0", port="4.3", id="prompt-mouse")
    guest.wait_log("0627:0001 mouse (slot", timeout=20)
    frame = guest.base / "prompt.ppm"
    qmp.call("screendump", filename=str(frame))
    with frame.open("rb") as data:
        assert data.readline().strip() == b"P6"
        width, height = [int(value) for value in data.readline().split()]

    def pointer():
        result=guest.call("hw_command",command="mouse")
        match=re.fullmatch(r"mouse: avail=1 x=(\d+) y=(\d+) btn=0x0+",result["reply"])
        assert match,result
        return int(match[1]),int(match[2])

    # The fixture uses the default 100% desktop scale.
    x, y = width // 2 + 100, (height - 48) // 2 + 36
    px,py=pointer()
    dx,dy=x-px,y-py
    while dx or dy:
        sx,sy=max(-100,min(100,dx)),max(-100,min(100,dy))
        qmp.call("input-send-event",events=[
            {"type":"rel","data":{"axis":"x","value":sx}},
            {"type":"rel","data":{"axis":"y","value":sy}},
        ])
        time.sleep(.1)
        dx-=sx
        dy-=sy
    assert pointer()==(x,y)
    for down in (True, False):
        qmp.call("input-send-event", events=[{"type":"btn","data":{"button":"left","down":down}}])
        time.sleep(.1)


def clean_exit(guest, request, *, automatic):
    states = []
    deadline = time.monotonic() + 100
    while guest.process.poll() is None and time.monotonic() < deadline:
        try:
            state=power(guest,"status",request)
        except RuntimeError:
            if guest.process.poll() is None:
                time.sleep(.25)
            continue
        states.append(state)
        assert state["state"] not in ("failed","cancelled","rejected"), state
        if state["state"] == "ready" and not automatic:
            power(guest,"commit",request)
        time.sleep(.25)
    assert guest.process.wait(timeout=10) == 0, states
    if automatic:
        assert "USB_SYSTEM_FINAL_SYNC result=0 clean=1" in guest.serial.read_text(errors="replace")
    return states


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("files","store","kconsole-image","operator-seed","output"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--accel",choices=("kvm","tcg"),default="kvm")
    args=parser.parse_args()
    if args.accel=="kvm" and subprocess.run(["pgrep","-x","vmware-vmx"],capture_output=True).returncode==0:
        parser.error("Workstation is active")
    args.output=args.output.resolve()
    args.output.mkdir(mode=0o700,parents=True)
    fixture.adopt_prebuilt(args.files)
    system,fallback=fixture.make_disks(args)
    hashes=(fixture.digest_outside_system(system),fixture.sha(fallback))
    proof={}
    guest=fixture.Guest(args,system,fallback,"desktop")
    try:
        guest.wait_log("USB_SYSTEM_READY")
        guest.wait_log("BOOT: desktop presented")
        guest.wait_log("CLEARWATER_SHELL_READY")
        guest.attach()
        request=uuid.uuid4().hex
        started=power(guest,"restart",request,reason="boot_update")
        assert started["state"]=="prompting" and started["prompt_state"]=="prompting", started
        click_not_now(guest)
        rejected=power(guest,"status",request)
        assert rejected["state"]=="rejected" and rejected["prompt_state"]=="rejected", rejected
        assert rejected["restart_pending"] and guest.process.poll() is None
        proof["not_now"]=[started,rejected]
        request=uuid.uuid4().hex
        when=time.monotonic()
        started=power(guest,"restart",request,reason="software_update")
        assert started["state"]=="prompting", started
        proof["timeout"]=[started,*clean_exit(guest,request,automatic=True)]
        assert time.monotonic()-when >= started["prompt_seconds"]-1
        assert any(row["prompt_state"]=="timeout" for row in proof["timeout"]), proof["timeout"]
    finally:
        guest.stop()
        (args.output/"verdict.json").write_text(json.dumps(proof,indent=2)+"\n")
    assert hashes==(fixture.digest_outside_system(system),fixture.sha(fallback))
    # Boot a disposable copy of the prompt-capable kconsole set.
    console=args.output/"kconsole.raw"
    subprocess.run(["cp","--reflink=auto","--sparse=always",args.kconsole_image,console],check=True)
    guest=fixture.Guest(args,console,fallback,"kconsole")
    try:
        guest.wait_log("BRIDGE endpoint tcp")
        guest.attach()
        request=uuid.uuid4().hex
        started=power(guest,"restart",request)
        assert started["state"]=="closing" and started["prompt_state"]=="bypassed", started
        proof["kconsole"]=[started,*clean_exit(guest,request,automatic=False)]
        assert all(row["state"]!="prompting" for row in proof["kconsole"])
    finally:
        guest.stop()
        (args.output/"verdict.json").write_text(json.dumps(proof,indent=2)+"\n")


if __name__=="__main__":
    main()
