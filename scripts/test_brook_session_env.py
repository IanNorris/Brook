"""Host check of the environment sourced by each brook-run launcher."""

import os
from pathlib import Path
import stat
import subprocess
from tempfile import TemporaryDirectory

SESSION_ENV = Path(__file__).resolve().parents[1] / "data/nix-wrappers/brook-session-env.sh"


def launch(root, **overrides):
    return subprocess.run(
        ["bash", "-c", 'source "$1" "$2" || exit; exec env', "brook-run",
         str(SESSION_ENV), str(root)],
        env={"PATH": os.environ["PATH"], **overrides},
        capture_output=True,
        text=True,
    )


with TemporaryDirectory(prefix="brook-session-test-") as temporary:
    root = Path(temporary)
    runtime = root / f"brook-{os.geteuid()}"

    result = launch(root, XDG_RUNTIME_DIR=str(root), XDG_SESSION_TYPE="x11")
    assert result.returncode == 0, result.stderr
    env = dict(line.split("=", 1) for line in result.stdout.splitlines())
    assert env["XDG_SESSION_TYPE"] == "wayland"
    assert env["XDG_CONFIG_DIRS"] == "/nix/profile/etc/xdg:/etc/xdg"
    assert env["GSETTINGS_SCHEMA_DIR"] == "/nix/profile/share/brook/gsettings"
    assert env["XDG_RUNTIME_DIR"] == str(runtime)
    assert env["WAYLAND_DISPLAY"] == str(root / "wayland-0")
    assert env["QT_QPA_PLATFORMTHEME"] == "kde" and env["KDE_SESSION_VERSION"] == "6"
    assert runtime.stat().st_uid == os.geteuid()
    assert stat.S_IMODE(runtime.stat().st_mode) == 0o700

    runtime.chmod(0o2755)
    result = launch(root, QT_QPA_PLATFORMTHEME="custom", XDG_CONFIG_DIRS="/test/xdg",
                    GSETTINGS_SCHEMA_DIR="/test/schemas")
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert "QT_QPA_PLATFORMTHEME=custom" in lines
    assert "XDG_CONFIG_DIRS=/test/xdg" in lines
    assert "GSETTINGS_SCHEMA_DIR=/test/schemas" in lines
    assert stat.S_IMODE(runtime.stat().st_mode) == 0o700

    runtime.rmdir()
    target = root / "target"
    target.mkdir()
    target.chmod(0o755)
    runtime.symlink_to(target, target_is_directory=True)
    result = launch(root)
    assert result.returncode != 0 and "private runtime directory" in result.stderr
    assert stat.S_IMODE(target.stat().st_mode) == 0o755
    assert not result.stdout
