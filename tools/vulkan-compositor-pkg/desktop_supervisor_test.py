import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="brook-supervisor-test-") as tmp:
    root = Path(tmp)
    child = root / "renderer"
    child.write_text("#!/bin/sh\n"
                     "echo x >> \"$BROOK_TEST_STARTS\"\n"
                     "test \"$(wc -l < \"$BROOK_TEST_STARTS\")\" -gt 1 || exit 7\n"
                     "echo $$ > \"$BROOK_TEST_PID\"\n"
                     "exec sleep 30\n")
    child.chmod(0o755)
    env = dict(os.environ, BROOK_TEST_STARTS=str(root / "starts"), BROOK_TEST_PID=str(root / "pid"))
    proc = subprocess.Popen([sys.argv[1], str(child)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and not (root / "pid").exists():
            if proc.poll() is not None:
                raise AssertionError("supervisor exited before restart")
            time.sleep(0.05)
        assert (root / "pid").exists(), "renderer was not restarted"
        pid = int((root / "pid").read_text())
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=5) == 0
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("renderer survived supervisor shutdown")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        proc.stderr.close()
print("DESKTOP_SUPERVISOR_RESTART_PASS")

with tempfile.TemporaryDirectory(prefix="brook-supervisor-partial-") as tmp:
    root=Path(tmp);child=root/"renderer";seen=root/"seen"
    child.write_text('#!/bin/sh\nprintf "%s\\n" "$BROOK_RENDER_PARTIAL" > "$BROOK_TEST_SEEN"\nexec sleep 30\n')
    child.chmod(0o755)
    proc=subprocess.Popen([sys.argv[1],"--partial",str(child)],env=dict(os.environ,BROOK_TEST_SEEN=str(seen)),stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while time.monotonic()<deadline and not seen.exists():time.sleep(0.05)
        assert seen.read_text().strip()=="1"
        proc.send_signal(signal.SIGTERM);assert proc.wait(timeout=5)==0
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        proc.stderr.close()
print("DESKTOP_SUPERVISOR_PARTIAL_PASS")

with tempfile.TemporaryDirectory(prefix="brook-supervisor-async-") as tmp:
    root=Path(tmp);child=root/"renderer";seen=root/"seen"
    child.write_text('#!/bin/sh\nprintf "%s %s %s\\n" "$BROOK_RENDER_ASYNC_PARTIAL" "$BROOK_RENDER_BUFFER_COUNT" "$BROOK_RENDER_ASYNC_PARTIAL_GRACE" > "$BROOK_TEST_SEEN"\nexec sleep 30\n')
    child.chmod(0o755)
    env = dict(os.environ, BROOK_TEST_SEEN=str(seen), BROOK_RENDER_BUFFER_COUNT="3")
    env.pop("BROOK_RENDER_ASYNC_PARTIAL_GRACE", None)
    cases = (("1", env), ("0", dict(env, BROOK_RENDER_ASYNC_PARTIAL_GRACE="0")))
    for expected_grace, child_env in cases:
        proc = subprocess.Popen([sys.argv[1], "--async-partial", str(child)], env=child_env, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic()+5
            while time.monotonic()<deadline and not seen.exists():
                time.sleep(0.05)
            assert seen.read_text().strip() == "1 2 " + expected_grace
            proc.send_signal(signal.SIGTERM)
            assert proc.wait(timeout=5) == 0
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            proc.stderr.close()
        seen.unlink()
assert subprocess.run([sys.argv[1],"--async-partial","--partial","/bin/true"],capture_output=True).returncode==2
print("DESKTOP_SUPERVISOR_ASYNC_PARTIAL_PASS")

with tempfile.TemporaryDirectory(prefix="brook-supervisor-source-") as tmp:
    root=Path(tmp);child=root/"renderer";seen=root/"seen"
    child.write_text('#!/bin/sh\nprintf "%s %s %s %s\\n" "$BROOK_RENDER_PARTIAL" "$BROOK_DESKTOP_GPU_SOCKET" "$BROOK_RENDER_SELECTIVE_CAPTURE" "$BROOK_RENDER_GPU_SOURCE_TRACE" > "$BROOK_TEST_SEEN"\nexec sleep 30\n')
    child.chmod(0o755)
    proc=subprocess.Popen([sys.argv[1],"--partial","--gpu-source","/tmp/test.sock","--trace-gpu-source",str(child)],env=dict(os.environ,BROOK_TEST_SEEN=str(seen)),stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+5
        while time.monotonic()<deadline and not seen.exists():time.sleep(0.05)
        assert seen.read_text().strip()=="1 /tmp/test.sock 1 1"
        proc.send_signal(signal.SIGTERM);assert proc.wait(timeout=5)==0
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        proc.stderr.close()
assert subprocess.run([sys.argv[1],"--gpu-source","relative","/bin/true"],capture_output=True).returncode==2
assert subprocess.run([sys.argv[1],"--trace-gpu-source","/bin/true"],capture_output=True).returncode==2
print("DESKTOP_SUPERVISOR_SOURCE_PASS")
