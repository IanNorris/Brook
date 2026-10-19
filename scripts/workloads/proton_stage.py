#!/usr/bin/env python3
"""Stage a private Proton runtime for Brook; never modifies the source tree.

Run on the host. The output must be a new directory below the repo's build/.
Only runtime packaging changes are made: Nix interpreter paths and optional
preloader disabling. Windows DLLs and the Proton launcher remain unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--wine", required=True, type=Path,
                        help="Existing guest Wine Nix output supplying the runtime closure")
    parser.add_argument("--patchelf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--disable-preloaders", action="store_true")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    wine = args.wine.resolve(strict=True)
    output = args.output.resolve()
    build = Path(__file__).resolve().parents[2] / "build"
    output.relative_to(build.resolve())
    if output == build.resolve() or output.exists():
        parser.error("Output must be a new experiment directory below build/")
    if output.is_relative_to(source):
        parser.error("Output must not be inside the source distribution")
    for name in ("proton", "version", "files/bin/wineserver"):
        if not (source / name).is_file():
            parser.error("Source must be an extracted Proton distribution")
    patcher = args.patchelf.resolve(strict=True)
    wine_loader = wine / "bin/.wine"  # The pinned Wine package wraps bin/wine.
    if not wine_loader.is_file():
        wine_loader = wine / "bin/wine"
    loader = subprocess.check_output(
        [str(patcher), "--print-interpreter", str(wine_loader)], text=True).strip()
    closure = subprocess.check_output(["nix-store", "-qR", str(wine)], text=True).splitlines()
    output.mkdir(parents=True)
    destination = output / "distribution"
    shutil.copytree(source, destination, symlinks=True)
    for path in [destination, *destination.rglob("*")]:
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | 0o200 | (0o100 if path.is_dir() else 0))
    patches, disabled = [], []
    for path in destination.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            tag = stream.read(5)
        if tag != b"\x7fELF\x02":
            continue
        result = subprocess.run([str(patcher), "--print-interpreter", str(path)],
                                capture_output=True, text=True)
        if result.returncode == 0:
            before = digest(path)
            subprocess.run([str(patcher), "--set-interpreter", loader, str(path)], check=True)
            patches.append(dict(file=str(path.relative_to(destination)),
                                original_sha256=before, sha256=digest(path),
                                original_interpreter=result.stdout.strip()))
    if args.disable_preloaders:
        for path in destination.rglob("*preloader"):
            if path.is_file() or path.is_symlink():
                disabled.append(str(path.relative_to(destination)))
                path.rename(path.with_name(path.name + ".brook-disabled"))
    libraries = {}
    for entry in closure:
        for path in sorted((Path(entry) / "lib").glob("*.so*")):
            if path.is_file():
                libraries.setdefault(path.name, str(path))
    (output / "runtime-libraries.json").write_text(json.dumps(libraries, indent=2) + "\n")
    manifest = dict(source=str(source), version=(source / "version").read_text().strip(),
                    loader=loader, wine=str(wine), interpreter_patches=patches,
                    optional_preloaders_disabled=disabled)
    (output / "packaging.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def guest_owner(info):
        info.uid, info.gid = 1000, 100
        info.uname, info.gname = "brook", "users"
        return info

    with tarfile.open(output / "proton.tar", "w") as archive:
        archive.add(destination, arcname="proton", filter=guest_owner)
    print(json.dumps(dict(output=str(output), archive_bytes=(output / "proton.tar").stat().st_size,
                          patched_interpreters=len(patches), disabled_preloaders=disabled)))


if __name__ == "__main__":
    main()
