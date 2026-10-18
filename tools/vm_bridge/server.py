#!/usr/bin/env python3
"""Brook VM-only MCP over newline JSON-RPC; also usable as a JSON CLI.

No host desktop APIs, input daemons, clipboard, or arbitrary host shell tool.
"""
import argparse
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid
import wave
import zlib

from common import ROOT, REGISTRY, QMP, artifact, load_session, process_identity, process_is_alive, recording_lock, session_lock, write_json
from media import Delivery, PreviewLimitError, describe as describe_artifact, scoped_path
from jobs import Jobs
from ui import Ui
from observation import Observations, StaleObservation, validate_selection
from frame_views import FrameViews
import reset as reset_control

MAX_LOG = 256 * 1024


def tail(path, maximum=MAX_LOG):
    with Path(path).open("rb") as f:
        f.seek(0, 2)
        f.seek(max(0, f.tell() - maximum))
        return f.read().decode(errors="replace")


def panic_state(data):
    with Path(data["serial_log"]).open("rb") as log:
        size = log.seek(0, 2)
        log.seek(max(data.get("generation_log_offset", 0), size - 65536))
        text = log.read().decode(errors="replace")
    return bool(re.search(r"KERNEL PANIC|KERNEL FAULT", text))


def parse_guest_reply(reply):
    # A census is a framed JSON-lines stream, not just its first header.
    # Keep single-object responses and surrounding human logs compatible.
    lines = [line for line in reply.splitlines() if line.startswith("{")]
    if not lines:
        return {"reply": reply.strip()}
    first = json.loads(lines[0])
    if first.get("kind") != "memory_census":
        return first
    records = [json.loads(line) for line in lines[1:]]
    if not records or records[-1].get("snapshot") != "rolling" or "spaces" not in records[-1]:
        raise ValueError("Incomplete memory census response")
    return {**first, "records": records[:-1], "summary": records[-1]}


def guest(data, command, timeout=8):
    if panic_state(data):
        raise RuntimeError("Guest has panicked; use vm_debug for QEMU-level inspection")
    if "\n" in command or "\r" in command or len(command.encode()) > 220:
        raise ValueError("Guest command must fit one 220-byte protocol line")
    token = str(int(uuid.uuid4().hex[:12], 16))
    begin, end = f"BRIDGE_BEGIN {token}", f"BRIDGE_END {token}"
    deadline = time.monotonic() + timeout
    with socket.create_connection(("127.0.0.1", data["guest_debug_port"]), timeout=timeout) as s:
        s.sendall(f"@{token} {command}\n".encode())
        buffer = bytearray()
        while time.monotonic() < deadline:
            s.settimeout(max(.1, deadline - time.monotonic()))
            chunk = s.recv(65536)
            if not chunk:
                break
            buffer.extend(chunk)
            if len(buffer) > 2 * 1024 * 1024:
                raise ValueError("Guest reply exceeded bounded receive buffer")
            text = buffer.decode(errors="replace")
            start = text.find(begin + "\n")
            # Replies use LF; the log stream may use CRLF.
            if start >= 0 and end + "\n" in text[start:]:
                reply = text[start + len(begin) + 1:].split(end + "\n", 1)[0]
                return parse_guest_reply(reply)
    raise TimeoutError("No complete guest response; delivery/completion is unknown (not retried)")


def png_rgb(width, height, rgb):
    if len(rgb) != width * height * 3:
        raise ValueError("Invalid RGB extent")
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value) & 0xffffffff)
    rows = b"".join(b"\0" + rgb[y*width*3:(y+1)*width*3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


@contextlib.contextmanager
def completed_frame_source(session, data, boot_id=None):
    """A guest-owned immutable frame lease, never a mutable render buffer."""
    def command(suffix):
        prefix='bridge frame ' if boot_id is None else f'bridge frame guarded {boot_id} '
        result=guest(data,prefix+suffix,timeout=10)
        if 'frame_error' in result:raise RuntimeError(result['frame_error'])
        return result
    source = command("request")
    if not isinstance(source, dict) or source.get("capture_api") != 2:
        raise RuntimeError("Kernel lacks completed-frame capture; boot a current build")
    token = source.get("token")
    if not isinstance(token, int) or isinstance(token, bool) or not 0 < token <= 0xffffffff:
        raise RuntimeError("Completed-frame capture is busy or unavailable")
    try:
        deadline = time.monotonic() + 10
        while source.get("state") == 1:
            if time.monotonic() >= deadline:
                raise TimeoutError("Compositor did not publish a completed frame")
            time.sleep(.01)
            source = command(f"status {token}")
            if source.get("capture_api") != 2 or source.get("token") != token:
                raise RuntimeError("Completed-frame token changed while waiting")
        if source.get("state") != 2:
            raise RuntimeError("Completed-frame capture failed or expired")
        yield source
        # Guest time was stopped during RAM extraction, but the lease could
        # expire before/after it. Never publish bytes from an invalidated lease.
        status = command(f"status {token}")
        if any(status.get(key) != source.get(key) for key in
               ("capture_api", "token", "state", "frame_epoch", "address", "width", "height", "stride")):
            raise RuntimeError("Completed-frame lease expired during capture")
    finally:
        failed = sys.exc_info()[0] is not None
        try:
            released = command(f"release {token}")
            if released.get("released") is not True:
                raise RuntimeError("Completed-frame lease release was not acknowledged")
        except (OSError, ValueError, RuntimeError, TimeoutError):
            if not failed:
                raise


def read_frame_bytes(session, source):
    w, h, stride = source["width"], source["height"], source["stride"]
    address = int(source["address"], 16)
    if not address or not (0 < w <= 4096 and 0 < h <= 2160 and w <= stride <= 8192):
        raise ValueError("Guest framebuffer metadata is invalid")
    size = stride * h * 4
    if size > 64 * 1024 * 1024:
        raise ValueError("Framebuffer exceeds capture limit")
    raw_path = artifact(session, ".xrgb")
    try:
        with QMP(session, timeout=20) as q:
            running = q.call("query-status")["running"]
            if running:
                q.call("stop")
            try:
                q.hmp(f'memsave {address:#x} {size} "{raw_path}"')
            finally:
                if running:
                    q.call("cont")
        raw = raw_path.read_bytes()
        if len(raw) != size:
            raise RuntimeError("QEMU returned an incomplete framebuffer")
        return raw
    finally:
        raw_path.unlink(missing_ok=True)


def capture(session, data, crop=None, max_width=None, boot_id=None):
    """Read a leased completed frame; panic keeps the cached physical fallback."""
    metadata_path = REGISTRY / session / "frame-source.json"
    panic = panic_state(data)
    if panic:
        if not metadata_path.exists():
            raise RuntimeError("No pre-panic framebuffer metadata; memory/register inspection is available")
        source = json.loads(metadata_path.read_text())
        if source.get("session_generation") is not None and source["session_generation"]!=data["generation"]:
            raise RuntimeError("Cached panic geometry belongs to another host generation")
        source["address"] = source["panic_address"]
        source["stride"] = source["panic_stride"]
        raw = read_frame_bytes(session, source)
    else:
        with completed_frame_source(session, data, boot_id) as source:
            raw = read_frame_bytes(session, source)
        source={**source,"session_generation":data["generation"],"guest_boot_id":boot_id}
        write_json(metadata_path, source)
    w, h, stride = source["width"], source["height"], source["stride"]
    rgb = bytearray(w*h*3)
    for y in range(h):
        row = raw[y*stride*4:y*stride*4+w*4]
        offset = y*w*3
        rgb[offset:offset+w*3:3] = row[2::4]
        rgb[offset+1:offset+w*3:3] = row[1::4]
        rgb[offset+2:offset+w*3:3] = row[0::4]
    frame = {"generation": data["generation"],
             "guest_boot_id":boot_id,
             "coordinate_width": w, "coordinate_height": h,
             "captured_monotonic": time.monotonic(),
             "source": "panic_cpu_framebuffer" if panic else "guest_completed_frame",
             "frame_epoch": None if panic else source.get("frame_epoch"),
             "frame_tick_ms": None if panic else source.get("frame_tick_ms"),
             "panic": panic,
             "geometry_provenance":{"session_generation":source.get("session_generation"),
                 "guest_boot_id":source.get("guest_boot_id"),"verified_for_current_boot":bool(boot_id and not panic),
                 "note":"Panic geometry may be from an earlier guest boot" if panic else "Boot-guarded lease" if boot_id else "Legacy unguarded lease"},
             "intrusive": "completed-frame copy and brief VM pause; GPU readback at frame boundary"}
    result=frame_views(session).publish(bytes(rgb),frame,crop,max_width)
    write_json(REGISTRY / session / "last-frame.json",result['data'])
    return result


def frame_views(session):
    return FrameViews(REGISTRY,session,png_rgb,lambda suffix:artifact(session,suffix))


def frame_view(session,frame_id,crop=None,max_width=None,size='full',overlay=False,**_):
    data=load_session(session,live=False)
    result=frame_views(session).view(frame_id,crop,max_width,size,overlay)
    result['data']['current_generation']=data['generation']
    result['data']['historical_generation']=result['data']['generation']!=data['generation']
    return result


def sessions(**_):
    rows = []
    for p in sorted(REGISTRY.glob("*/session.json")):
        try:
            data = json.loads(p.read_text())
            load_session(data["session"])
            data["attachable"] = True
        except (OSError, ValueError, KeyError):
            try:
                data = json.loads(p.read_text()); data["attachable"] = False
            except (OSError, ValueError):
                continue
        rows.append(data)
    return {"sessions": rows}


def doctor(session, **_):
    with session_lock(session) as data, QMP(session) as q:
        result = {"session": session, "qemu": q.version, "vm": q.call("query-status"),
                  "panic": panic_state(data), "gpu": data["gpu"], "render_node": data["render_node"],
                  "provenance": data["provenance"], "cloned_disks": data["cloned_disks"],
                  "commands": [x["name"] for x in q.call("query-commands")
                               if x["name"] in {"input-send-event","screendump","human-monitor-command",
                                                "query-status","query-blockstats","stop","cont","quit"}],
                  "capture": "leased completed frame + QEMU memory; requires current kernel or cached panic metadata",
                  "kernel_panic_pipe": "unavailable", "guest_widget_tree": "unavailable",
                  "generation": data["generation"], "server_version": "0.9.1",
                  "bridge_capabilities": {"artifact_delivery": {"revision": 1,
                      "default": "references", "include_image": True,
                      "preview_max_bytes": 8*1024*1024, "preview_max_images": 4}}}
        result['bridge_capabilities']['frame_views']={'revision':1,'tiny_width':320,'medium_width':960,'cached_crops':True,'coordinate_overlay':True,'crop_coordinates':'native_frame','extra_vm_capture':False}
        result['bridge_capabilities']['verified_reset']={'revision':1,'launch_reboot_action':data.get('qemu_reboot_action'),
            'recovery_tool':'vm_reset_status','verification':'Running QMP and different nonzero guest boot; missing prior epoch remains unverified'}
        result['bridge_capabilities']['observation_bundles']={'revision':1,'components':['frame','windows','processes','launches','errors'],
            'atomic':False,'frame_window_correspondence':'unverified','errors_source':'bounded serial-pattern scan'}
        try:
            result["guest"] = guest(data, "bridge stats", timeout=3)
        except (OSError, ValueError, RuntimeError) as error:
            result["guest_error"] = str(error)
        try:
            caps=guest(data,"bridge job caps",timeout=3)
            result["bridge_capabilities"]["command_jobs"]={"supported":caps.get("job_api")==2,**caps}
        except (OSError,ValueError,RuntimeError) as error:
            result["bridge_capabilities"]["command_jobs"]={"supported":False,"error":str(error)}
        try:
            caps=guest(data,"bridge ui caps",timeout=3)
            result["bridge_capabilities"]["window_controls"]={"supported":caps.get("ui_api")==1,**caps}
        except (OSError,ValueError,RuntimeError) as error:
            result["bridge_capabilities"]["window_controls"]={"supported":False,"error":str(error)}
        return result


def start(scenario="desktop", gpu="software", **_):
    if scenario not in {"desktop", "video", "quake-soft", "quake-gl3", "office"}:
        raise ValueError("Unknown named experiment")
    if gpu not in {"software", "gl"}:
        raise ValueError("Unsupported GPU profile")
    before = {p.parent.name for p in REGISTRY.glob("*/session.json")}
    REGISTRY.mkdir(parents=True, exist_ok=True, mode=0o700)
    log_path = REGISTRY / ("start-" + uuid.uuid4().hex + ".log")
    env = dict(os.environ, BROOK_VM_BRIDGE="1", BROOK_VM_PROVENANCE="agent",
               BROOK_AUDIODEV="none", BROOK_EXPERIMENT_TIMEOUT="3600")
    if gpu == "gl":
        env.update(BROOK_GPU="gl", BROOK_COMPOSITE="gpu")
    else:
        env.update(BROOK_GPU="0", BROOK_COMPOSITE="cpu")
    # A fresh instance port avoids colliding with user-started VMs.
    for instance in range(100, 200):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", 11237 + instance*100))
                break
            except OSError:
                continue
    else:
        raise RuntimeError("No free VM debug port")
    env["BROOK_INSTANCE"] = str(instance)
    with log_path.open("wb") as out:
        subprocess.Popen(["nix-shell", "--run", f"exec ./scripts/run_experiment.sh {scenario}"],
                         cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        for p in REGISTRY.glob("*/session.json"):
            if p.parent.name not in before:
                data = load_session(p.parent.name)
                if data["provenance"] == "agent" and data["guest_debug_port"] == 11237+instance*100:
                    return {"session": data["session"], "launch_log": str(log_path), "guest_booting": True}
        time.sleep(.1)
    return {"launch_log": str(log_path), "pending": True,
            "message": "Launch continues independently; discover it with vm_sessions"}


def control(session, action, force=False, wait_s=20, _cancel=None, **_):
    if not isinstance(force, bool):
        raise ValueError("force must be a boolean")
    if force and action not in {"stop", "reset"}:
        raise ValueError("force is only supported for stop/reset")
    if isinstance(wait_s,bool) or not isinstance(wait_s,(int,float)) or not 0<=wait_s<=20:
        raise ValueError('wait_s must be 0–20')
    if action in {"takeover", "release"}:
        load_session(session)
        ui=window_ui()
        # One transition lock across clients orders host intent and the guest
        # latch. Individual guest exchanges take session_lock themselves.
        with ui.lock(REGISTRY/session/'control-transition',deadline=time.monotonic()+6):
            write_json(REGISTRY / session / "control.json", {"user_takeover": action == "takeover"})
            result={"user_takeover": action == "takeover"}
            try:result['guest_ui']=ui.takeover(session,action=='takeover')
            except (OSError,RuntimeError,ValueError) as error:result['guest_ui_unavailable']=str(error)
            return result
    with session_lock(session) as data:
        if action == "panic":
            try:
                guest(data, "exec /boot/SCRIPTS/KPANIC.RC", timeout=2)
            except TimeoutError:
                pass
            return {"panic_observed":panic_state(data),"message":"Requested controlled panic; inspect logs/debug state. Not retried."}
        commands = {"pause": "stop", "resume": "cont", "reset": "system_reset", "stop": "quit"}
        if action not in commands:
            raise ValueError("Unknown control action")
        sync_result = {}
        if action in {"stop", "reset"}:
            # Callers must quiesce writers first: the guest can write again
            # between this acknowledgement and the following QMP command.
            try:
                reply = guest(data, "bridge sync", timeout=30)
                if not isinstance(reply, dict) or reply.get("guest_sync_completed") is not True:
                    raise RuntimeError("Guest did not acknowledge completed metadata sync")
                sync_result = {"guest_sync_completed": True,
                               "sync_guarantee": reply.get("sync_guarantee",
                                                           "best_effort_metadata_flush")}
            except (OSError, RuntimeError, ValueError) as error:
                if not force:
                    raise RuntimeError(
                        f"{action} aborted before QMP: {error}. "
                        "For panic/unreachable guests, force=true explicitly permits "
                        "shutdown without completed sync and possible filesystem corruption."
                    ) from error
                sync_result = {"guest_sync_completed": False, "forced": True,
                               "sync_error": str(error)}
        if action == "reset":
            return {"action":action,"session":session,**sync_result,
                **reset_control.perform(session,data,REGISTRY,guest,QMP,wait_s,_cancel)}
        with QMP(session) as q:q.call(commands[action])
        return {"action": action, "session": session, **sync_result}


def reset_status(session,**_):
    data=load_session(session,live=False)
    if data['state']!='running' or not process_is_alive(data['pid'],data['process_identity']):
        return {'session':session,**reset_control.status(session,data,REGISTRY,guest,QMP,available=False)}
    try:
        with session_lock(session) as data:
            return {'session':session,**reset_control.status(session,data,REGISTRY,guest,QMP)}
    except (OSError,ValueError):
        data=load_session(session,live=False)
        if process_is_alive(data['pid'],data['process_identity']):raise
        return {'session':session,**reset_control.status(session,data,REGISTRY,guest,QMP,available=False)}


def observe(session, crop=None, max_width=None, include=None, since_event_cursor=None,
            error_limit_bytes=65536,error_limit_events=64, **_):
    if include is not None:
        return Observations(window_ui(),capture,panic_state).collect(session,include,crop,max_width,
            since_event_cursor,error_limit_bytes,error_limit_events)
    if since_event_cursor is not None:raise ValueError('since_event_cursor requires include=[errors]')
    with session_lock(session) as data:
        return capture(session, data,crop,max_width)


def logs(session, source="serial", offset=0, limit=65536, **_):
    if source not in {"serial", "qemu"} or not 0 <= offset or not 1 <= limit <= MAX_LOG:
        raise ValueError("Invalid log source/cursor/limit")
    data = load_session(session, live=False)
    path = data[f"{source}_log"]
    with open(path, "rb") as f:
        size = f.seek(0, 2)
        rotated = offset > size
        f.seek(0 if rotated else offset)
        chunk = f.read(limit)
        next_offset = f.tell()
    return {"text": chunk.decode(errors="replace"), "next_offset": next_offset,
            "more": next_offset < size, "rotated": rotated, "source": source}


def stats(session, memory_details=False, audio_details=False, **_):
    with session_lock(session) as data:
        result = guest(data, "bridge stats")
        if memory_details:
            started = time.monotonic()
            result["memory_census"] = guest(data, "mem census", timeout=30)
            result["memory_census"]["supported"] = result["memory_census"].get("kind") == "memory_census"
            result["memory_census"]["round_trip_ms"] = (time.monotonic() - started) * 1000
        if audio_details:
            started = time.monotonic()
            result["audio"] = guest(data, "audio stats")
            result["audio"]["supported"] = (isinstance(result["audio"].get("streams"), list)
                                             and "guest_clock_ns" in result["audio"])
            result["audio"]["round_trip_ms"] = (time.monotonic() - started) * 1000
        result["queried_monotonic"] = time.monotonic()
        result["note"] = "Recent 256 compositor-loop intervals, nearest-rank percentiles, millisecond guest timer resolution; not app GPU or scanout frametimes"
        return result


def window_ui():
    return Ui(REGISTRY,load_session,session_lock,guest,write_json)


def apps(session,**_):
    return window_ui().apps(session)


def windows(session,**_):
    return window_ui().windows(session)


def action_observation_options(session,include=None,crop=None,max_width=None,since_event_cursor=None,
                               error_limit_bytes=65536,error_limit_events=64,include_image=False,target_generation=None):
    if include is None:
        if crop is not None or max_width is not None or since_event_cursor is not None or include_image or error_limit_bytes!=65536 or error_limit_events!=64:
            raise ValueError('Observation options require include')
        return None
    validate_selection(include,crop,max_width,since_event_cursor,error_limit_bytes,error_limit_events)
    if since_event_cursor is not None and (since_event_cursor['session']!=session or
            (target_generation is not None and since_event_cursor['generation']!=target_generation)):
        raise ValueError('Stale error cursor session/generation')
    return {'include':include,'crop':crop,'max_width':max_width,'cursor':since_event_cursor,
        'error_limit_bytes':error_limit_bytes,'error_limit_events':error_limit_events}


def attach_action_observation(session,result,options,cancel=None):
    if options is None:return result
    identity={'generation':result['generation'],'guest_boot_id':result['guest_boot_id']}
    if cancel is not None and cancel.is_set():
        bundle={'state':'cancelled','expected_identity':identity,'error':'Wait cancelled before post-action observation'}
    else:
        try:
            bundle=Observations(window_ui(),capture,panic_state).collect(session,**options,
                expected_generation=identity['generation'],expected_boot_id=identity['guest_boot_id'])
        except (OSError,ValueError,RuntimeError) as error:
            bundle={'state':'stale' if isinstance(error,StaleObservation) else 'unavailable',
                'expected_identity':identity,'error':str(error)}
    return {**result,'post_action_observation':bundle,
        'post_action_observation_scope':'Separate read after the wait; action conditions were sampled earlier; neither window presence nor pixels prove application readiness'}


def window_action(session,window,action,request_id,restore_target=None,wait_for='applied',wait_s=2,_cancel=None,
                  include=None,crop=None,max_width=None,since_event_cursor=None,error_limit_bytes=65536,error_limit_events=64,include_image=False,**_):
    options=action_observation_options(session,include,crop,max_width,since_event_cursor,error_limit_bytes,error_limit_events,include_image,
        target_generation=window.get('session_generation') if isinstance(window,dict) else None)
    result=window_ui().submit(session,window,action,request_id,restore_target,wait_for,wait_s,_cancel)
    return attach_action_observation(session,result,options,_cancel)


def launch_app(session,app,request_id,wait_for='window',wait_s=2,_cancel=None,
               include=None,crop=None,max_width=None,since_event_cursor=None,error_limit_bytes=65536,error_limit_events=64,include_image=False,**_):
    options=action_observation_options(session,include,crop,max_width,since_event_cursor,error_limit_bytes,error_limit_events,include_image,
        target_generation=app.get('session_generation') if isinstance(app,dict) else None)
    result=window_ui().launch(session,app,request_id,wait_for,wait_s,_cancel)
    return attach_action_observation(session,result,options,_cancel)


def ui_status(session,operation_id,wait_s=0,wait_for='applied',_cancel=None,
              include=None,crop=None,max_width=None,since_event_cursor=None,error_limit_bytes=65536,error_limit_events=64,include_image=False,**_):
    options=action_observation_options(session,include,crop,max_width,since_event_cursor,error_limit_bytes,error_limit_events,include_image)
    result=window_ui().status(session,operation_id,wait_s,wait_for,_cancel)
    return attach_action_observation(session,result,options,_cancel)


def command_jobs():
    return Jobs(REGISTRY,load_session,session_lock,guest,write_json)


def job_status(session,operation_id,stdout_offset=0,stderr_offset=0,output_limit=4096,wait_s=0,_cancel=None,**_):
    return command_jobs().status(session,operation_id,stdout_offset,stderr_offset,output_limit,wait_s,_cancel)


def guest_run(session, command=None, argv=None, cwd="/data/home", env=None, uid=1000,
              request_id=None, wait_s=0, _cancel=None, **_):
    if argv is not None:
        if command is not None:raise ValueError("Choose structured argv or legacy command, not both")
        return command_jobs().submit(session,request_id,argv,cwd,env,uid,wait_s,_cancel)
    if request_id is not None or env is not None or cwd!="/data/home" or uid!=1000 or wait_s!=0:
        raise ValueError("Job options require structured argv")
    # This is explicitly the Brook run-command grammar, not host shell code.
    if not isinstance(command, str) or not command.startswith("run "):
        raise ValueError("Use Brook launch syntax: run [--uid 1000] /absolute/guest/path args")
    with session_lock(session) as data:
        result = guest(data, "bridge run " + command)
        if not result.get("launched"):
            raise RuntimeError("Guest failed to launch the command")
        result["completion"] = "Launch acknowledgement only; stdout/stderr remain in guest/serial logs"
        return result


def profile_event_summary(content):
    """Expose producer losses separately from export/conversion success."""
    begin = re.findall(r"^PROF_BEGIN \d+ \d+\s*$", content, re.M)
    end = re.findall(r"^PROF_END (\d+) (\d+)\s*$", content, re.M)
    if len(begin) != 1 or len(end) != 1:
        raise RuntimeError("Profile needs one complete begin/end capture")
    start = content.index("PROF_BEGIN ")
    finish = content.index("PROF_END ")
    if finish <= start:
        raise RuntimeError("Profile end precedes begin")
    records = content[start:finish]
    samples = len(re.findall(r"^P ", records, re.M))
    switches = len(re.findall(r"^CS ", records, re.M))
    reported, dropped = map(int, end[0])
    matches = samples + switches == reported
    return {"sample_count": samples, "context_switch_count": switches,
            "reported_event_count": reported, "dropped_event_count": dropped,
            "event_count_matches": matches, "lossy": dropped > 0 or not matches,
            "quality_note": "Producer drops and count mismatches limit interpretation; IRQ-masked time is not sampled uniformly"}


def profile(session, action="status", duration_ms=1000, guest_timeout=8, **_):
    if action not in {"start", "stop", "status", "export"} or not 100 <= duration_ms <= 30000:
        raise ValueError("Invalid profile action/duration")
    with session_lock(session) as data:
        def query(command):
            return guest(data, command, timeout=guest_timeout)
        if action=="start" and "RUNNING" in query("prof status").get("reply",""):
            raise RuntimeError("A profiler capture is already recording or flushing")
        if action != "export":
            command = f"prof start {duration_ms}" if action == "start" else "prof " + action
            return query(command)
        state = query("prof status")
        if "RUNNING" in state.get("reply", ""):
            raise RuntimeError("Profiler recording/flushing: export after status reports IDLE")
        text = tail(data["serial_log"])
        matches = re.findall(r"PROFILER: writing to (/boot/[A-Za-z0-9_.]+)", text)
        if not matches:
            raise RuntimeError("No recorded profile output path in recent serial log")
        source = "::" + Path(matches[-1]).name
        path = artifact(session, ".profile")
        command = "exec mcopy -o -i " + shlex.quote(data["boot_disk"]) + " " + shlex.quote(source) + " " + shlex.quote(str(path))
        result = subprocess.run(["nix-shell", "--run", command], cwd=ROOT, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[-2048:])
        content = path.read_text(errors="replace")
        summary = profile_event_summary(content)
        output = path.with_suffix(".speedscope.json")
        if hashlib.sha256(Path(data["kernel"]).read_bytes()).hexdigest() != data["kernel_sha256"]:
            raise RuntimeError("Saved run kernel changed; refusing incorrect profile symbolization")
        converted = subprocess.run([sys.executable, str(ROOT / "scripts/profiler_to_speedscope.py"),
                                    str(path), str(output), "--elf", data["kernel"]],
                                   capture_output=True, timeout=30)
        return {"profile": str(path), "speedscope": str(output) if converted.returncode == 0 else None,
                **summary,
                "kernel_sha256": data["kernel_sha256"], "conversion_error": converted.stderr.decode(errors="replace")[-2048:]}


def ram_ranges(tree):
    # FlatView numbers depend on devices and QEMU version; use the system AS.
    views = re.split(r"(?m)^FlatView #\d+", tree)
    system = next((v for v in views if re.search(r'AS "memory", root: system', v)), None)
    if system is None:
        raise RuntimeError("QEMU system memory map unavailable")
    return re.findall(r"([0-9a-f]{16})-([0-9a-f]{16}) \(prio \d+, ram\)", system)


def debug(session, action="registers", address="0x0", size=256, cpu=0, **_):
    if action not in {"registers", "physical_memory", "virtual_memory"} or not 1 <= size <= 1024*1024 or not 0 <= cpu < 256:
        raise ValueError("Invalid diagnostic operation/range")
    addr = int(address, 0)
    if not 0 <= addr <= 2**64-size:
        raise ValueError("Invalid memory address")
    with session_lock(session) as data, QMP(session) as q:
        if cpu not in {c["cpu-index"] for c in q.call("query-cpus-fast")}:
            raise ValueError("No such vCPU in this VM")
        if action == "registers":
            return {"registers": q.hmp("info registers", cpu=cpu), "cpu": cpu,
                    "context": "current vCPU; original fault registers remain in panic logs", "panic": panic_state(data)}
        running = q.call("query-status")["running"]
        if running:
            q.call("stop")
        try:
            ranges = ram_ranges(q.hmp("info mtree -f"))
            if action == "physical_memory":
                if not any(int(a,16) <= addr and addr+size-1 <= int(b,16) for a,b in ranges):
                    raise ValueError("Range is not wholly inside an advertised QEMU RAM region")
            else:
                start=addr
                while start<addr+size:
                    translated=q.hmp(f"gva2gpa {start:#x}", cpu=cpu)
                    match=re.search(r"gpa: (0x[0-9a-f]+)",translated)
                    if not match:raise ValueError("Virtual address cannot be translated")
                    physical=int(match[1],16)
                    count=min(4096-(start%4096),addr+size-start)
                    if not any(int(a,16)<=physical and physical+count-1<=int(b,16) for a,b in ranges):
                        raise ValueError("Virtual read crosses non-RAM memory")
                    start+=count
            path = artifact(session, ".memory")
            command = "pmemsave" if action == "physical_memory" else "memsave"
            q.hmp(f'{command} {addr:#x} {size} "{path}"', cpu=cpu)
            blob = path.read_bytes()
            if len(blob) != size:
                raise RuntimeError("Memory export incomplete")
            return {"artifact": str(path), "address": address, "size": size, "cpu": cpu,
                    "address_space": action, "sha256": hashlib.sha256(blob).hexdigest(),
                    "hex_preview": blob[:1024].hex(), "full_dump": False,
                    "consistency": "vCPUs paused during translation and read; device activity may continue"}
        finally:
            if running:
                q.call("cont")



KEYS = {"ENTER":"ret", "RETURN":"ret", "ESC":"esc", "ESCAPE":"esc", "CTRL":"ctrl",
        "CONTROL":"ctrl", "ALT":"alt", "SHIFT":"shift", "SUPER":"meta_l",
        "ARROWUP":"up", "ARROWDOWN":"down", "ARROWLEFT":"left", "ARROWRIGHT":"right",
        "BACKSPACE":"backspace", "TAB":"tab", "SPACE":"spc", "DELETE":"delete"}
PUNCT = {" ":("spc",False), "\n":("ret",False), "\t":("tab",False)}
for plain, shifted, key in [("-","_","minus"),("=","+","equal"),("[","{","bracket_left"),
                           ("]","}","bracket_right"),(";",":","semicolon"),("'",'"',"apostrophe"),
                           (",","<","comma"),(".",">","dot"),("/","?","slash"),("\\","|","backslash"),
                           ("`","~","grave_accent")]:
    PUNCT[plain]=(key,False);PUNCT[shifted]=(key,True)
for digit, shifted in zip("1234567890","!@#$%^&*()"):
    PUNCT[shifted]=(digit,True)


def text_keys(text):
    result=[]
    for c in text:
        if c.isascii() and c.isalnum():result.append((c.lower(), c.isupper()))
        elif c in PUNCT:result.append(PUNCT[c])
        else:raise ValueError("Unsupported character; no input was sent. Initial text backend uses US ASCII layout")
    return result


def input_actions(session, actions, frame_id=None, _cancel=None, **_):
    if not isinstance(actions,list) or not 1 <= len(actions) <= 64:
        raise ValueError("Provide 1–64 ordered input actions")
    if any(not isinstance(a,dict) for a in actions):raise ValueError("Actions must be objects")
    if sum(float(a.get("seconds",0)) for a in actions)>20:
        raise ValueError("Input waits exceed 20 seconds")
    for a in actions:
        if a.get("type") not in {"keypress","type","move","relative_move","click","double_click","scroll","key_down","key_up","button_down","button_up","wait"}:
            raise ValueError("Unknown action type")
        if a["type"]=="type":text_keys(a.get("text",""))
        if set(a)-{"type","text","keys","key","button","x","y","dx","dy","seconds","hold_seconds"}:
            raise ValueError("Unknown action field")
        if "hold_seconds" in a:
            hold=a["hold_seconds"]
            if a["type"] not in {"click","double_click","keypress"}:
                raise ValueError("hold_seconds applies to click, double_click or keypress")
            if isinstance(hold,bool) or not isinstance(hold,(int,float)) or not math.isfinite(hold) or not .01<=hold<=1:
                raise ValueError("hold_seconds must be finite and between 0.01 and 1")
        if a["type"]=="keypress" and (not isinstance(a.get("keys"),list) or not 1<=len(a["keys"])<=8 or any(not isinstance(k,str) for k in a["keys"])):
            raise ValueError("keypress requires 1–8 key names")
        if a["type"] in {"key_down","key_up"} and not isinstance(a.get("key"),str):raise ValueError("Key name required")
        for k in {"x","y","dx","dy"}&set(a):
            if isinstance(a[k],bool) or not isinstance(a[k],int):raise ValueError("Coordinates/deltas must be integers")
        if "seconds" in a and (not isinstance(a["seconds"],(int,float)) or not math.isfinite(a["seconds"])):
            raise ValueError("Wait must be finite")
        if a.get("button","left") not in {"left","middle","right"}:raise ValueError("Invalid pointer button")
        if a["type"]=="scroll" and any(abs(int(a.get(k,0)))>100 for k in ["dx","dy"]):raise ValueError("Scroll count exceeds 100")
        if a["type"]=="relative_move" and any(abs(int(a.get(k,0)))>32767 for k in ["dx","dy"]):raise ValueError("Relative movement exceeds limit")
        if a["type"]=="wait" and not 0 <= float(a.get("seconds",0)) <= 5:raise ValueError("Wait must be 0–5 seconds")
    if sum(len(a.get("text","")) for a in actions)>512:raise ValueError("Text batch exceeds 512 characters")
    # Mouse buttons are sampled by Brook's compositor, so a 20 ms pulse can
    # disappear entirely at 8–13 FPS. Hold normal clicks across several frames.
    timed=sum(float(a.get("seconds",0)) for a in actions)
    timed+=sum(float(a.get("hold_seconds",.2 if a["type"] in {"click","double_click"} else .02)) *
               (2 if a["type"]=="double_click" else 1)
               for a in actions if a["type"] in {"click","double_click","keypress"})
    if timed>20:raise ValueError("Input waits and holds exceed 20 seconds")
    with session_lock(session) as data:
        lease=REGISTRY/session/"control.json"
        if lease.exists() and json.loads(lease.read_text()).get("user_takeover"):
            raise RuntimeError("User owns control; input blocked")
        if panic_state(data):raise RuntimeError("Normal input is disabled after guest panic")
        frame_path=REGISTRY/session/"last-frame.json"
        frame=json.loads(frame_path.read_text()) if frame_path.exists() else None
        coordinates=any(a["type"] in {"move","click","double_click","scroll"} for a in actions)
        if coordinates:
            if not frame:raise ValueError("Observe first and pass that frame_id")
            if frame['frame_id']!=frame_id:
                candidate=frame_views(session).metadata(frame_id)
                if not frame.get('source_frame_id') or candidate['source_frame_id']!=frame['source_frame_id']:
                    raise ValueError('Frame view is not from the latest capture')
                frame=candidate
            if frame.get('generation') is not None and data.get('generation') is not None and frame['generation']!=data['generation']:
                raise ValueError('Stale frame session generation')
            if frame.get('guest_boot_id') is not None:
                epoch=guest(data,'bridge epoch')
                if epoch.get('boot_id')!=frame['guest_boot_id']:raise ValueError('Stale frame guest boot')
        for a in actions:
            if a["type"] in {"move","click","double_click","scroll"}:
                if not 0 <= int(a.get("x",-1)) < frame["width"] or not 0 <= int(a.get("y",-1)) < frame["height"]:
                    raise ValueError("Coordinate is outside the referenced frame")
        held_keys=set();held_buttons=set()
        deadline=time.monotonic()+30
        def check_cancel():
            if time.monotonic()>deadline:raise TimeoutError("Input batch deadline exceeded")
            if _cancel and _cancel.is_set():raise RuntimeError("Input cancelled")
            if lease.exists() and json.loads(lease.read_text()).get("user_takeover"):
                raise RuntimeError("User took over control")
        with QMP(session) as q:
            # QMP schema type names are opaque (often numeric), not C names.
            valid_keys=next({m["name"] for m in x.get("members",[])}
                            for x in q.call("query-qmp-schema")
                            if x.get("meta-type")=="enum" and
                            {"ret","ctrl","shift"}.issubset({m["name"] for m in x.get("members",[])}))
            def keyname(k):return KEYS.get(k.upper(),k.lower())
            for a in actions:
                for k in a.get("keys",[a.get("key")] if "key" in a else []):
                    if keyname(k) not in valid_keys:raise ValueError("Unsupported QEMU key: "+k)
            def send(events):q.call("input-send-event",{"events":events})
            def key(k, down):
                send([{"type":"key","data":{"down":down,"key":{"type":"qcode","data":k}}}])
                (held_keys.add if down else held_keys.discard)(k)
            def button(b,down):
                send([{"type":"btn","data":{"down":down,"button":b}}])
                (held_buttons.add if down else held_buttons.discard)(b)
            def hold_for(seconds):
                end=time.monotonic()+seconds
                while time.monotonic()<end:
                    check_cancel();time.sleep(min(.05,max(0,end-time.monotonic())))
            def chord(keys,hold_seconds=.02):
                introduced=[k for k in keys if k not in held_keys]
                for k in introduced:key(k,True)
                hold_for(hold_seconds)
                for k in reversed(introduced):key(k,False)
            try:
                for a in actions:
                    check_cancel()
                    kind=a["type"]
                    if kind in {"move","click","double_click","scroll"}:
                        gx=frame["crop_origin"][0]+int(a["x"])*frame.get("crop_width",frame["width"])//frame["width"]
                        gy=frame["crop_origin"][1]+int(a["y"])*frame.get("crop_height",frame["height"])//frame["height"]
                        send([{"type":"abs","data":{"axis":"x","value":round(gx*32767/max(1,frame["coordinate_width"]-1))}},
                              {"type":"abs","data":{"axis":"y","value":round(gy*32767/max(1,frame["coordinate_height"]-1))}}])
                    if kind in {"click","double_click"}:
                        for _i in range(2 if kind=="double_click" else 1):
                            button(a.get("button","left"),True);hold_for(float(a.get("hold_seconds",.2)));button(a.get("button","left"),False);hold_for(.04)
                    elif kind=="scroll":
                        for direction,count in [("wheel-up" if int(a.get("dy",0))<0 else "wheel-down",abs(int(a.get("dy",0)))),
                                                ("wheel-left" if int(a.get("dx",0))<0 else "wheel-right",abs(int(a.get("dx",0))))]:
                            if count>100:raise ValueError("Scroll count exceeds 100")
                            for _i in range(count):button(direction,True);button(direction,False)
                    elif kind=="relative_move":
                        send([{"type":"rel","data":{"axis":"x","value":int(a.get("dx",0))}},
                              {"type":"rel","data":{"axis":"y","value":int(a.get("dy",0))}}])
                    elif kind=="keypress":chord([keyname(k) for k in a["keys"]],float(a.get("hold_seconds",.02)))
                    elif kind in {"key_down","key_up"}:key(keyname(a["key"]),kind=="key_down")
                    elif kind in {"button_down","button_up"}:button(a.get("button","left"),kind=="button_down")
                    elif kind=="type":
                        for k,shift in text_keys(a["text"]):
                            check_cancel();chord((["shift"] if shift else [])+[k])
                    elif kind=="wait":
                        end=time.monotonic()+float(a.get("seconds",0))
                        while time.monotonic()<end:
                            check_cancel();time.sleep(min(.05,max(0,end-time.monotonic())))
            finally:
                for k in list(held_keys):key(k,False)
                for b in list(held_buttons):button(b,False)
        return {"injected_actions":len(actions),"guest_reaction_verified":False,"holds_released":True}


def wait(session, text, timeout_s=5, _cancel=None, **_):
    if not isinstance(text,str) or not text or len(text)>256 or not 0 <= timeout_s <= 30:
        raise ValueError("Bounded nonempty substring and 0–30 second timeout required")
    data=load_session(session);end=time.monotonic()+timeout_s
    while time.monotonic()<end:
        if text in tail(data["serial_log"]):return {"matched":True,"text":text}
        if _cancel and _cancel.wait(.1):return {"cancelled":True}
        if not _cancel:time.sleep(.1)
    return {"matched":False,"timeout":True}


def video_recording(session,action="status",duration_s=30,fps=30,audio=True,width=1920,height=1080,**_):
    if action not in {"start","status","stop"}:raise ValueError("Use start/status/stop")
    descriptor=REGISTRY/session/"video-recording.json"
    # Validate session before forming/reading scoped recording paths.
    data=load_session(session,live=action!="status")
    if action=="status":
        if not descriptor.exists():return {"state":"idle","session":session}
        state=json.loads(descriptor.read_text())
        if state.get("state") in {"starting","recording"}:
            try:alive=process_is_alive(state["pid"],state["process_identity"])
            except (OSError,KeyError):alive=False
            if not alive and not (state.get("state")=="starting" and time.monotonic()-state.get("created_monotonic",0)<10):
                state.update(state="failed",error="Recorder process is no longer alive")
        return state
    with session_lock(session) as data, recording_lock(session):
        state=json.loads(descriptor.read_text()) if descriptor.exists() else {}
        active=False
        if state.get("state") in {"starting","recording"}:
            try:active=process_is_alive(state["pid"],state["process_identity"])
            except (OSError,KeyError):active=state.get("state")=="starting" and time.monotonic()-state.get("created_monotonic",0)<10
        if action=="stop":
            if not active:return {"state":state.get("state","idle"),"stop_requested":False}
            token=state.get("recording","")
            if not re.fullmatch(r"[0-9a-f]{32}",token):raise ValueError("Invalid recording descriptor")
            (REGISTRY/session/f"video-stop-{token}").touch()
            return {"recording":token,"stop_requested":True,"completion":"Poll status for finalized artifact"}
        if active:raise RuntimeError("A VM video recording is already active")
        if not isinstance(duration_s,(int,float)) or isinstance(duration_s,bool) or not math.isfinite(duration_s) or not 1<=duration_s<=600:raise ValueError("Recording duration must be 1–600 seconds")
        if not isinstance(fps,int) or isinstance(fps,bool) or not 1<=fps<=60:raise ValueError("Recording FPS must be 1–60")
        if not isinstance(audio,bool):raise ValueError("audio must be boolean")
        if any(not isinstance(v,int) or isinstance(v,bool) or v%2 for v in (width,height)) or not 32<=width<=4096 or not 32<=height<=2160:raise ValueError("Even recording canvas up to 4096x2160 required")
        runtime=ROOT/"build/vm-recording-runtime/bin/python3"
        if not runtime.exists():raise RuntimeError("Prepare runtime before VM launch: nix-build tools/vm_bridge/recording-runtime.nix -o build/vm-recording-runtime")
        token=uuid.uuid4().hex
        claim={"recording":token,"session":session,"generation":data["generation"],
               "state":"starting","created_monotonic":time.monotonic()}
        write_json(descriptor,claim)
        path=artifact(session,".record.log")
        args=[str(runtime),str(ROOT/"tools/vm_bridge/record.py"),"--session",session,
              "--recording",token,"--duration",str(duration_s),"--fps",str(fps),
              "--width",str(width),"--height",str(height),"--wait-for-parent"]+(["--audio"] if audio else [])
        worker=None
        try:
            with path.open("wb") as output:
                worker=subprocess.Popen(args,cwd=ROOT,stdout=output,stderr=subprocess.STDOUT,
                                        stdin=subprocess.PIPE,start_new_session=True)
            claim.update(pid=worker.pid,process_identity=process_identity(worker.pid))
            write_json(descriptor,claim)
            worker.stdin.write(b"start\n");worker.stdin.close()
            threading.Thread(target=worker.wait,daemon=True).start()
        except BaseException as error:
            if worker:
                if worker.stdin and not worker.stdin.closed:worker.stdin.close()
                if worker.poll() is None:worker.terminate()
                worker.wait(timeout=10)
            write_json(descriptor,{**claim,"state":"failed","error":str(error)});raise
        return {"recording":token,"state":"starting","pid":worker.pid,"log":str(path),
                "completion":"Poll vm_record status; start acknowledgement is not a finalized video"}


def audio_capture(session, duration_s=2, _cancel=None, **_):
    if not 0.1 <= duration_s <= 10:raise ValueError("Capture must be 0.1–10 seconds")
    with session_lock(session) as data,QMP(session) as q:
        path=artifact(session,".wav")
        before=q.hmp("info capture")
        q.hmp(f'wavcapture "{path}" hda0 44100 16 2')
        info=q.hmp("info capture")
        indices=re.findall(r"(?:^|\n)\[?(\d+)\]?:",info)
        old=set(re.findall(r"(?:^|\n)\[?(\d+)\]?:",before))
        new=[x for x in indices if x not in old]
        if len(new)!=1:raise RuntimeError("Could not identify the new QEMU audio capture")
        began=time.monotonic()
        recording=REGISTRY/session/"recording.json"
        write_json(recording,{"index":int(new[0]),"artifact":str(path),"deadline":began+duration_s+2})
        try:
            if _cancel:_cancel.wait(duration_s)
            else:time.sleep(duration_s)
        finally:
            q.hmp("stopcapture "+new[0])
            recording.unlink(missing_ok=True)
        ended=time.monotonic()
        with wave.open(str(path)) as f:
            params=f.getparams()
        return {"artifact":str(path),"sample_rate":params.framerate,"channels":params.nchannels,
                "sample_width":params.sampwidth,"frames":params.nframes,
                "sample_duration_s":params.nframes/params.framerate,
                "host_monotonic_interval":[began,ended],"backend":"qemu-wavcapture",
                "chunk_arrival_timestamps":False,"guest_queue_telemetry":False,"speaker_playback_required":False,
                "stream_active":params.nframes>0}


def guest_inspect(session, action="windows", pid=0, **_):
    commands={"windows":"wm list","processes":"procs"}
    if action=="focus":
        if not 0<=pid<128:raise ValueError("Window index outside range")
        command="wm focus "+str(pid)
    elif action in commands:command=commands[action]
    else:raise ValueError("Use windows/processes/focus; pid selects a window index for focus")
    with session_lock(session) as data:return guest(data,command)


def get_artifact(session, artifact_name, **_):
    load_session(session,live=False)
    descriptor=describe_artifact(REGISTRY,session,artifact_name)
    result={"artifact":descriptor["path"],"size":descriptor["byte_length"],**descriptor}
    if _.get("include_image",False) and descriptor["mime_type"]=="image/png":
        if descriptor["byte_length"]>8*1024*1024:
            raise ValueError("Image preview exceeds delivery limit; use artifact references")
        return {"image":Path(descriptor["path"]).read_bytes(),"data":result}
    return result


def audio_analyze(session, artifact_name, **_):
    name=Path(artifact_name).name
    if name!=artifact_name or not name.endswith(".wav"):raise ValueError("Use a WAV artifact basename")
    load_session(session,live=False);path=scoped_path(REGISTRY,session,name)
    if path.stat().st_size>8*1024*1024:raise ValueError("WAV exceeds analysis limit")
    with wave.open(str(path)) as f:
        channels,rate,width=f.getnchannels(),f.getframerate(),f.getsampwidth()
        if width!=2 or channels not in {1,2}:raise ValueError("Analysis currently supports PCM16 mono/stereo")
        raw=f.readframes(f.getnframes())
    samples=struct.unpack("<"+"h"*(len(raw)//2),raw)
    mono=samples[::channels];bins=800
    peaks=[max((abs(x) for x in mono[i*len(mono)//bins:(i+1)*len(mono)//bins]),default=0) for i in range(bins)]
    rms=math.sqrt(sum(x*x for x in mono)/max(1,len(mono)))/32768
    rgb=bytearray([245]*(bins*240*3))
    for x,peak in enumerate(peaks):
        extent=round(peak/32768*110)
        for y in range(120-extent,121+extent):
            rgb[(y*bins+x)*3:(y*bins+x)*3+3]=b"\x22\x76\xb8"
    image=png_rgb(bins,240,rgb);out=artifact(session,".waveform.png");out.write_bytes(image)
    return {"image":image,"data":{"waveform":str(out),"pcm":str(path),"rms":rms,
                                  "duration_s":len(mono)/rate,"plot":"time runs left-to-right; amplitude centered at y=120",
                                  "silent_bins":[i for i,p in enumerate(peaks) if p==0],
                                  "diagnosis":"Silence alone does not prove a stutter; guest timing counters remain necessary"}}


def schema(properties=None, required=None):
    return {"type":"object","properties":properties or {},"required":required or [],"additionalProperties":False}


STR={"type":"string"};NUM={"type":"number"};INT={"type":"integer"}
SESSION={"session":STR}
IMAGE_OPTION={"include_image":{"type":"boolean","default":False,
    "description":"Opt in to bounded native MCP image blocks; default returns local artifact references"}}
TOOLS={}
def register(name,fn,description,props=None,required=None,readonly=False):
    TOOLS[name]=(fn,{"name":name,"description":description,"inputSchema":schema(props,required),
                    "annotations":{"readOnlyHint":readonly,"destructiveHint":not readonly,"openWorldHint":False}})
register("vm_sessions",sessions,"Discover opted-in user/agent VM runs; no desktop discovery",readonly=True)
register("vm_doctor",doctor,"VM transport and guest readiness; capabilities distinguish unsupported paths",SESSION,["session"],True)
register("vm_start",start,"Boot a named experiment on new disk clones with playback off",{"scenario":STR,"gpu":STR})
register("vm_control",control,"pause/resume/reset/stop or takeover/release; stop/reset await guest sync; reset verifies a different guest boot, never infer restart from QMP acknowledgement; force explicitly permits unsynced shutdown",{**SESSION,"action":STR,"force":{"type":"boolean"},"wait_s":NUM},["session","action"])
register("vm_reset_status",reset_status,"Read-only recovery of the last reset: QMP state and changed guest boot identity; never resets/resumes or changes QEMU policy",SESSION,["session"],True)
OBSERVATION_OPTIONS={**IMAGE_OPTION,
    "include":{"type":"array","items":{"type":"string","enum":["frame","windows","processes","launches","errors"]},"minItems":1,"maxItems":5},
    "since_event_cursor":{"type":"object"},"error_limit_bytes":INT,"error_limit_events":INT,
    "crop":schema({"x":INT,"y":INT,"width":INT,"height":INT},["x","y","width","height"]),"max_width":INT}
register("vm_observe",observe,"Completed-frame artifact or selectable boot-bound frame/windows/tasks/launches/error bundle with per-component timing; snapshots are not atomic; serial errors are best-effort",{**SESSION,**OBSERVATION_OPTIONS},["session"])
register("vm_frame",frame_view,"Read a saved frame as tiny/medium/full or a native-coordinate crop; no VM capture or pause; views retain capture provenance and work after stop",{**SESSION,**IMAGE_OPTION,
    "frame_id":STR,"size":{"type":"string","enum":["tiny","medium","full"]},
    "overlay":{"type":"boolean","default":False,"description":"Add a diagnostic ruler/grid labelled in native-frame coordinates; clean source retained"},
    "crop":schema({"x":INT,"y":INT,"width":INT,"height":INT},["x","y","width","height"]),"max_width":INT},["session","frame_id"],True)
register("vm_logs",logs,"Read bounded logs by byte cursor",{**SESSION,"source":STR,"offset":INT,"limit":INT},["session"],True)
register("vm_stats",stats,"OS memory/process counts and compositor intervals; optional cached audio queues and rolling memory census (census holds guest mapping locks)",{**SESSION,"memory_details":{"type":"boolean"},"audio_details":{"type":"boolean"}},["session"],True)
register("vm_guest_run",guest_run,"Run structured guest argv with separate output and real exit status; requires request_id. Legacy command uses Brook run syntax and returns launch ACK only",{**SESSION,"command":STR,
    "argv":{"type":"array","items":STR,"minItems":1,"maxItems":64},"cwd":STR,
    "env":{"type":"object","additionalProperties":STR,"maxProperties":64},"uid":INT,
    "request_id":STR,"wait_s":NUM},["session"])
register("vm_job",job_status,"Recover/wait for a guest operation by ID; return exit, descendants, separate byte-cursor output and EOF; never repeats execution",{**SESSION,
    "operation_id":STR,"stdout_offset":INT,"stderr_offset":INT,"output_limit":INT,"wait_s":NUM},["session","operation_id"],True)
WINDOW_REF=schema({"window_id":INT,"incarnation":STR,"session_generation":STR,"guest_boot_id":STR},["window_id","incarnation","session_generation","guest_boot_id"])
register("vm_apps",apps,"Read the published installed-app/shortcut catalog, build-time availability and boot/catalog-bound references; no launches",SESSION,["session"],True)
register("vm_windows",windows,"Snapshot native/Wayland windows with boot-bound references, provider/client identities, geometry and supported actions",SESSION,["session"],True)
register("vm_window",window_action,"Apply a semantic compositor action once; return request status and windows. Close dispatch, window disappearance and exact client-group exit are separate conditions; optional include adds a post-wait bundle",{**SESSION,**OBSERVATION_OPTIONS,
    "window":WINDOW_REF,"action":{"type":"string","enum":list(Ui.actions)},"request_id":STR,
    "restore_target":{"type":"string","enum":list(Ui.restores)},"wait_for":{"type":"string","enum":["applied","closed","client_group_exit"]},"wait_s":NUM},["session","window","action","request_id"])
APP_REF=schema({"app_id":STR,"catalog_generation":INT,"session_generation":STR,"guest_boot_id":STR},["app_id","catalog_generation","session_generation","guest_boot_id"])
register("vm_launch",launch_app,"Launch an exact installed-app reference through the normal UI launcher once; return durable dispatch history and windows, without claiming app readiness; optional include adds a post-wait bundle",{**SESSION,**OBSERVATION_OPTIONS,
    "app":APP_REF,"request_id":STR,"wait_s":NUM,"wait_for":{"type":"string","enum":["admitted","dispatched","window"]}},["session","app","request_id"])
register("vm_ui",ui_status,"Recover a semantic UI request by ID without replaying it; optionally wait and return windows and an optional post-wait bundle",{**SESSION,**OBSERVATION_OPTIONS,"operation_id":STR,"wait_s":NUM,
    "wait_for":{"type":"string","enum":["applied","closed","client_group_exit","admitted","dispatched","window"]}},["session","operation_id"],True)
register("vm_guest",guest_inspect,"Guest windows/processes, or focus by window index in pid",{**SESSION,"action":STR,"pid":INT},["session"])
register("vm_artifacts",get_artifact,"Describe a scoped artifact by local path, size and checksum, including after VM exit; binary is never embedded in JSON",{**SESSION,**IMAGE_OPTION,"artifact_name":STR},["session","artifact_name"],True)
register("vm_profile",profile,"CPU samples and context-switch capture: start/stop/status/export to Speedscope",{**SESSION,"action":STR,"duration_ms":INT},["session"])
register("vm_debug",debug,"QEMU-level current registers or bounded physical/virtual memory; works independently of guest TCP",{**SESSION,"action":STR,"address":STR,"size":INT,"cpu":INT},["session"],True)
register("vm_input",input_actions,"Ordered timed key/mouse batches, ASCII-US text; all held state released at end/cancel",{**SESSION,"actions":{"type":"array","minItems":1,"maxItems":64,"items":schema({
    "type":{"type":"string","enum":["keypress","type","move","relative_move","click","double_click","scroll","key_down","key_up","button_down","button_up","wait"]},
    "x":INT,"y":INT,"dx":INT,"dy":INT,"button":{"type":"string","enum":["left","middle","right"]},
    "key":STR,"keys":{"type":"array","items":STR},"text":STR,"seconds":NUM,
    "hold_seconds":{"type":"number","minimum":.01,"maximum":1,"description":"Click/double-click press duration (default0.2s); keypress override (default0.02s)"}},["type"])},"frame_id":STR},["session","actions"])
register("vm_wait",wait,"Wait for a serial-log substring without repeatedly returning whole logs",{**SESSION,"text":STR,"timeout_s":NUM},["session","text"],True)
register("vm_record",video_recording,"Direct VM video/audio recording: start/status/stop; private display stream, no host desktop or microphone",{**SESSION,"action":STR,"duration_s":NUM,"fps":INT,"audio":{"type":"boolean"},"width":INT,"height":INT},["session"])
register("vm_audio_capture",audio_capture,"Bounded PCM WAV capture without speaker output; no host microphone",{**SESSION,"duration_s":NUM},["session"])
register("vm_audio_analyze",audio_analyze,"Return waveform artifact and metrics for a session WAV artifact",{**SESSION,**IMAGE_OPTION,"artifact_name":STR},["session","artifact_name"],True)


def call_tool(name,arguments,cancel=None):
    if name not in TOOLS:raise ValueError("Unknown tool")
    fn,spec=TOOLS[name];properties=spec["inputSchema"]["properties"]
    if any(k not in properties for k in arguments):raise ValueError("Unknown argument")
    if any(k not in arguments for k in spec["inputSchema"]["required"]):raise ValueError("Missing required argument")
    for k,v in arguments.items():
        kind=properties[k]["type"]
        if (kind=="string" and not isinstance(v,str)) or (kind=="boolean" and not isinstance(v,bool)) or \
           (kind=="array" and not isinstance(v,list)) or (kind=="object" and not isinstance(v,dict)) or (kind in {"integer","number"} and
            (isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v) or (kind=="integer" and not isinstance(v,int)))):
            raise ValueError("Invalid type for "+k)
    result=fn(**arguments,_cancel=cancel)
    delivery=Delivery(REGISTRY,arguments.get("session"),arguments.get("include_image",False))
    if isinstance(result,dict) and 'post_action_observation' in result:
        # Optional media delivery cannot erase a mutation's durable result.
        post=result['post_action_observation']
        result=delivery.adapt({k:v for k,v in result.items() if k!='post_action_observation'})
        try:
            extra=Delivery(REGISTRY,arguments.get('session'),arguments.get('include_image',False))
            try:adapted=extra.adapt(post)
            except PreviewLimitError as error:
                extra=Delivery(REGISTRY,arguments.get('session'),False)
                adapted=extra.adapt(post);adapted['preview_error']=str(error)
            result['post_action_observation']=adapted
            delivery.descriptors.update(extra.descriptors);delivery.images.extend(extra.images)
        except (OSError,ValueError,RuntimeError,TypeError) as error:
            result['post_action_observation']={'state':'unavailable','delivery_error':str(error),
                'expected_identity':post.get('expected_identity') if isinstance(post,dict) else None}
    else:result=delivery.adapt(result)
    if delivery.descriptors:
        result["artifacts"]=list(delivery.descriptors.values())
    content=delivery.images
    content.append({"type":"text","text":json.dumps(result)})
    return {"content":content,"isError":False}


def serve():
    out_lock=threading.Lock();pending={};pending_lock=threading.Lock()
    executor=ThreadPoolExecutor(max_workers=4)
    control_executor=ThreadPoolExecutor(max_workers=1)
    def emit(value):
        with out_lock:
            print(json.dumps(value,separators=(",",":")),flush=True)
    def handle(message,cancel):
        ident=message.get("id");method=message.get("method");params=message.get("params",{})
        try:
            if method=="initialize":
                requested=params.get("protocolVersion")
                version=requested if requested in {"2024-11-05","2025-03-26","2025-06-18"} else "2025-06-18"
                result={"protocolVersion":version,"capabilities":{"tools":{}},"serverInfo":{"name":"brook-vm","version":"0.9.1"}}
            elif method=="ping":result={}
            elif method=="tools/list":result={"tools":[spec for fn,spec in TOOLS.values()]}
            elif method=="tools/call":
                try:result=call_tool(params["name"],params.get("arguments",{}),cancel)
                except Exception as error:result={"content":[{"type":"text","text":str(error)}],"isError":True}
            else:
                emit({"jsonrpc":"2.0","id":ident,"error":{"code":-32601,"message":"Method not found"}});return
            emit({"jsonrpc":"2.0","id":ident,"result":result})
        except Exception as error:
            emit({"jsonrpc":"2.0","id":ident,"error":{"code":-32602,"message":str(error)}})
        finally:
            with pending_lock:pending.pop(ident,None)
    try:
        while True:
            raw=sys.stdin.buffer.readline(2*1024*1024+1)
            if not raw:break
            if len(raw)>2*1024*1024:
                emit({"jsonrpc":"2.0","id":None,"error":{"code":-32700,"message":"Message exceeds size limit"}});break
            try:message=json.loads(raw)
            except ValueError:
                emit({"jsonrpc":"2.0","id":None,"error":{"code":-32700,"message":"Invalid JSON"}});continue
            if not isinstance(message,dict) or not isinstance(message.get("method"),str) or not isinstance(message.get("params",{}),dict):
                emit({"jsonrpc":"2.0","id":None,"error":{"code":-32600,"message":"Invalid request"}});continue
            if message.get("method")=="notifications/cancelled":
                with pending_lock:
                    event=pending.get(message.get("params",{}).get("requestId"))
                    if event:event.set()
                continue
            if "id" not in message:continue
            if not isinstance(message["id"],(str,int)) or isinstance(message["id"],bool):
                emit({"jsonrpc":"2.0","id":None,"error":{"code":-32600,"message":"Invalid request ID"}});continue
            event=threading.Event()
            with pending_lock:
                if message["id"] in pending or len(pending)>=32:
                    emit({"jsonrpc":"2.0","id":message["id"],"error":{"code":-32600,"message":"Duplicate ID or too many pending requests"}});continue
                pending[message["id"]]=event
            pool=control_executor if message.get("method")=="tools/call" and message.get("params",{}).get("name")=="vm_control" else executor
            pool.submit(handle,message,event)
    finally:
        with pending_lock:
            for event in pending.values():event.set()
        executor.shutdown(wait=True)
        control_executor.shutdown(wait=True)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--call",choices=TOOLS)
    parser.add_argument("--arguments",default="{}")
    options=parser.parse_args()
    if options.call:
        try:print(json.dumps(call_tool(options.call,json.loads(options.arguments))),flush=True)
        except Exception as error:print(json.dumps({"error":str(error)}));raise SystemExit(1)
    else:serve()
