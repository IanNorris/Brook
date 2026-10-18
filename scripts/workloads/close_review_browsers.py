#!/usr/bin/env python3
"""Close review browsers and require their task census to drain before VM stop.

This is a browser-exit barrier, not a filesystem sync or VM power-off command.
It never signals tasks or treats closed windows alone as process completion.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/vm_bridge"))


def browser_window(window):
    name = window.get("app_id", "").lower().removesuffix(".desktop")
    return name in {"firefox", "firefox-esr", "org.mozilla.firefox", "chromium",
                    "chromium-browser", "google-chrome", "google-chrome-stable"}


def browser_tasks(tasks, launches):
    return [task for task in tasks if not task.get("kernel") and (
        str(task.get("launch_id", "0")) in launches or
        any(name in task.get("name", "").lower()
            for name in ("firefox", "chromium", "chrome_crashpad")))]


def close_browsers(bridge, session, output, deadline_s=60, clock=time.monotonic, sleep=time.sleep):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    deadline = clock() + deadline_s
    launches = set()
    requested = set()
    while True:
        observation = bridge.observe(session, include=["processes", "windows"])
        (output / "observation.json").write_text(json.dumps(observation, indent=2))
        parts = observation["components"]
        for name in ("processes", "windows"):
            if parts[name]["state"] != "ok":
                raise RuntimeError(f"{name} unavailable; browser exit is unverified")
        census = parts["processes"]["result"]
        if census.get("truncated"):
            raise RuntimeError("Task census truncated; browser exit is unverified")
        identity = {"session": session, "generation": observation.get("generation"),
                    "guest_boot_id": observation.get("guest_boot_id")}
        saved = output / "barrier.json"
        if saved.exists():
            previous = json.loads(saved.read_text())
            if previous["identity"] != identity:
                raise RuntimeError("Browser-exit record belongs to another guest epoch")
            launches.update(previous["launch_tokens"])
        # Retain lineage even if a window closes before this helper starts.
        for task in browser_tasks(census["tasks"], set()):
            token = str(task.get("launch_id", "0"))
            if token != "0":
                launches.add(token)
        windows = [w for w in parts["windows"]["result"]["windows"] if browser_window(w)]
        for window in windows:
            token = str(window.get("launch_id", "0"))
            if token != "0":
                launches.add(token)
            reference = window["reference"]
            key = (reference["window_id"], reference["incarnation"])
            if key in requested:
                continue
            request_file = output / f"close-{key[0]}-{key[1]}.json"
            if request_file.exists():
                request = json.loads(request_file.read_text())
                if request["reference"] != reference:
                    raise RuntimeError("Close record belongs to another guest epoch")
            else:
                request = {"reference": reference, "request_id": uuid.uuid4().hex}
                request_file.write_text(json.dumps(request, indent=2))
            request["result"] = bridge.window_action(session, reference, "close",
                request["request_id"], wait_for="applied", wait_s=2)
            request_file.write_text(json.dumps(request, indent=2))
            requested.add(key)
        remaining = browser_tasks(census["tasks"], launches)
        result = {"identity": identity, "browser_windows": len(windows), "remaining_tasks": remaining,
                  "launch_tokens": sorted(launches), "ready_for_checked_stop": not windows and not remaining,
                  "scope": "Rolling browser census only; sync, stopped-process check and offline fsck remain required"}
        (output / "barrier.json").write_text(json.dumps(result, indent=2))
        if result["ready_for_checked_stop"]:
            return result
        if clock() >= deadline:
            raise RuntimeError("Browser exit observation expired; VM left running. Inspect remaining tasks and resume.")
        sleep(.25)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--deadline", type=float, default=60)
    args = parser.parse_args()
    if not 1 <= args.deadline <= 300:
        parser.error("deadline must be 1–300 seconds")
    import server
    result = close_browsers(server, args.session, args.output, args.deadline)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
