"""Verify the boot supervisor restarts a killed shell and drains it on stop."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    shell = Path(directory) / "CWSHELL.ELF"
    shell.write_text(f"#!{sys.executable}\n"
                     "import os, signal, sys\n"
                     "print(f'SHELL_READY pid={os.getpid()}', file=sys.stderr, flush=True)\n"
                     "signal.pause()\n")
    shell.chmod(0o755)
    with subprocess.Popen([sys.argv[1], str(shell)], stderr=subprocess.PIPE,
                          start_new_session=True, bufsize=0) as service:
        def next_line():
            assert select.select([service.stderr], [], [], 10)[0], 'supervisor output timed out'
            return service.stderr.readline().decode().strip()

        def ready_shell():
            started, ready = sorted([next_line(), next_line()])
            assert started.startswith("CLEARWATER_SUPERVISOR_START pid="), started
            pid = int(started.rsplit("=", 1)[1])
            assert ready == f"SHELL_READY pid={pid}", ready
            return pid

        try:
            first_pid = ready_shell()
            os.kill(first_pid, signal.SIGKILL)
            assert next_line() == "CLEARWATER_SUPERVISOR_EXIT status=9 retry_s=1"
            second_pid = ready_shell()
            assert second_pid != first_pid
            service.terminate()
            assert service.wait(timeout=10) == 0
            try:
                os.kill(second_pid, 0)
            except ProcessLookupError:
                pass
            else:
                raise AssertionError("supervised shell survived termination")
        finally:
            if service.poll() is None:
                os.killpg(service.pid, signal.SIGKILL)
