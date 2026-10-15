#!/usr/bin/env python3
"""Build new, FUSE-free /data and /nix images for Brook app experiments."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(*args, capture=False):
    result = subprocess.run(args, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else None


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def install_package_tools(nix_root, manifest):
    """Build current sources, rather than copying stale checked-in binaries."""
    compiler = os.environ.get("MUSL_CC")
    if not compiler:
        raise RuntimeError("MUSL_CC is required; run this script inside nix-shell")
    for name in ["nix-search", "nix-install", "nix-fetch", "nar-unpack"]:
        target = nix_root / "bin" / name
        run(compiler, "-static", "-O2", "-o", str(target),
            str(ROOT / "tools" / name / (name + ".c")))
        manifest["assets"][name] = digest(target)
    (nix_root / "bin/nix").symlink_to("nix-install")
    stk_wrapper = ROOT / "data/nix-wrappers/stk-play.sh"
    for target in [nix_root / "stk-play.sh", nix_root / "bin/brook-supertuxkart"]:
        shutil.copy2(stk_wrapper, target)
        target.chmod(0o755)
    manifest["assets"]["stk-play.sh"] = digest(stk_wrapper)
    for name in ["curl", "xz", "zstd"]:
        (nix_root / "bin" / name).symlink_to("../profile/bin/" + name)
    (nix_root / "index").mkdir()
    index = nix_root / "index/packages.idx"
    shutil.copy2(ROOT / "tools/nix-index/packages.idx", index)
    manifest["assets"]["packages.idx"] = digest(index)
    for directory in ["var/cache/narinfo", "share/applications/icons"]:
        (nix_root / directory).mkdir(parents=True, exist_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/experiments/seeds")
    parser.add_argument("--video", type=Path)
    parser.add_argument("--q2-dir", type=Path, help="baseq2 folder containing pak0.pak")
    parser.add_argument("--q2-music", type=Path, help="OST folder with track*.ogg")
    parser.add_argument("--nix-gib", type=int, default=32)
    parser.add_argument("--data-gib", type=int, default=16)
    parser.add_argument("--bootstrap-mode", choices=["clean", "prepared"],
                        help="Minimal installer image, optionally including the bootstrap SDK")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any((output / name).exists() for name in ["data.img", "nix.img", "manifest.json"]):
        parser.error("Output already contains experiment disks; choose a new --output directory")
    if min(args.nix_gib, args.data_gib) < 1:
        parser.error("Disk sizes must be positive")
    if args.video and not args.video.is_file():
        parser.error("Video file does not exist")
    if args.q2_dir:
        pak = args.q2_dir / "pak0.pak"
        if not pak.is_file():
            parser.error("--q2-dir must contain pak0.pak")
        with pak.open("rb") as source:
            if source.read(4) != b"PACK":
                parser.error("pak0.pak has no Quake PACK header")
    if args.q2_music and not args.q2_music.is_dir():
        parser.error("Music directory does not exist")
    if args.bootstrap_mode and (args.video or args.q2_dir or args.q2_music):
        parser.error("Bootstrap images cannot include experiment media")

    if args.bootstrap_mode:
        profile = Path(run("nix-build", str(ROOT / "tools/bootstrap-base"),
                           "--arg", "withSdk", str(args.bootstrap_mode == "prepared").lower(),
                           "--no-out-link", capture=True))
    else:
        profile = Path(run("nix-build", str(ROOT / "nix/experiment-packages.nix"),
                           "-A", "profile", "--no-out-link", capture=True))
    closure = run("nix-store", "-qR", str(profile), capture=True).splitlines()
    transport_closure = set()
    if args.bootstrap_mode == "prepared":
        base = run("nix-build", str(ROOT / "tools/bootstrap-base"),
                   "--arg", "withSdk", "false", "--no-out-link", capture=True)
        transport_closure = set(run("nix-store", "-qR", base, capture=True).splitlines())
    manifest = {"revision": run("git", "-C", str(ROOT), "rev-parse", "HEAD", capture=True),
                "profile": str(profile), "closure": sorted(closure), "assets": {},
                "q2_data_present": bool(args.q2_dir),
                "bootstrap_mode": args.bootstrap_mode,
                "bootstrap_sdk_modes": "guest-nar" if args.bootstrap_mode == "prepared" else None,
                "working_tree_dirty": bool(run("git", "-C", str(ROOT), "status", "--porcelain", capture=True))}
    kernel = ROOT / "build/release/kernel/BROOK.elf"
    if kernel.is_file():
        manifest["kernel_sha256"] = digest(kernel)
    wallpaper = ROOT / "data/wallpaper.raw"
    source_image = ROOT / "data/wallpaper/brook-dawn.png"
    if source_image.is_file():
        run("python3", str(ROOT / "scripts/convert_wallpaper.py"),
            str(source_image), str(wallpaper))

    with tempfile.TemporaryDirectory(prefix=".stage-", dir=output) as temp:
        stage = Path(temp)
        nix_root, data_root = stage / "nix", stage / "data"
        for tree, marker in [(nix_root, "/nix"), (data_root, "/data")]:
            tree.mkdir(mode=0o755)
            (tree / "BROOK.MNT").write_text(marker)
        (nix_root / "store").mkdir()
        for index, path in enumerate(sorted(closure), 1):
            print(f"Copying store path {index}/{len(closure)}: {Path(path).name}", flush=True)
            run("cp", "-a", "--no-preserve=ownership", "--reflink=auto",
                path, str(nix_root / "store" / Path(path).name))
            if args.bootstrap_mode == "prepared" and path not in transport_closure:
                # Host stores use 0444/0555; nar-unpack currently creates
                # 0644/0755. Match installed-SDK mmap cache behavior in both
                # paths. Never follow symlinks back into the host store.
                copied = nix_root / "store" / Path(path).name
                if copied.is_symlink():
                    continue
                for entry in [copied, *copied.rglob("*")]:
                    if entry.is_symlink():
                        continue
                    mode = entry.stat().st_mode
                    entry.chmod(0o755 if stat.S_ISDIR(mode) or mode & 0o111 else 0o644)
        run("cp", "-a", "--no-preserve=ownership", str(profile), str(nix_root / "profile"))
        (nix_root / "bin").mkdir()
        install_package_tools(nix_root, manifest)
        if not args.bootstrap_mode:
            run("python3", str(ROOT / "scripts/collect_desktop_entries.py"),
                str(nix_root / "store"), str(nix_root / "share/applications"))
        (nix_root / "etc").mkdir(exist_ok=True)
        shutil.copy2(ROOT / "data/asound.conf", nix_root / "etc/asound.conf")
        shutil.copy2(ROOT / "data/config/alsoft-oss.conf", nix_root / "etc/alsoft-oss.conf")
        for executable in ([] if args.bootstrap_mode else ["waylandd", "wayland-calc", "brook-player", "brook-files",
                           "brook-office", "brook-ladybird", "brook-vlc",
                           "brook-yquake2-gl", "brook-yquake2-soft"]):
            source = profile / "bin" / executable
            if not source.exists():
                raise RuntimeError(f"Profile is missing {executable}")
            # Copy the actual executable; Brook sees an unambiguous exec path.
            shutil.copy2(source.resolve(), nix_root / "bin" / executable)
        cert = profile / "etc/ssl/certs/ca-bundle.crt"
        if cert.is_file():
            (nix_root / "etc/ssl/certs").mkdir(parents=True)
            shutil.copy2(cert, nix_root / "etc/ssl/certs/ca-bundle.crt")

        for directory in ["home", "yq2/.yq2/baseq2", "yq2/.cache", "yq2/.config", "tmp", "videos", "games/quake2/baseq2"]:
            (data_root / directory).mkdir(parents=True, exist_ok=True)
        if args.q2_dir:
            shutil.copytree(args.q2_dir, data_root / "games/quake2/baseq2", dirs_exist_ok=True)
            manifest["assets"]["pak0.pak"] = digest(args.q2_dir / "pak0.pak")
        controls = data_root / "games/quake2/baseq2/brook-controls.cfg"
        shutil.copy2(ROOT / "data/config/yquake2-controls.cfg", controls)
        manifest["assets"]["brook-controls.cfg"] = digest(controls)
        benchmark = data_root / "games/quake2/baseq2/brook-benchmark.cfg"
        shutil.copy2(ROOT / "data/config/yquake2-benchmark.cfg", benchmark)
        manifest["assets"]["brook-benchmark.cfg"] = digest(benchmark)
        manifest["assets"]["alsoft-oss.conf"] = digest(nix_root / "etc/alsoft-oss.conf")
        if args.q2_music:
            music = data_root / "games/quake2/baseq2/music"
            music.mkdir(parents=True, exist_ok=True)
            # Yamagi asks for trackNN.ogg on a case-sensitive guest filesystem.
            for track in args.q2_music.glob("*.[oO][gG][gG]"):
                shutil.copy2(track, music / track.name.lower())
            if not any(music.iterdir()):
                raise RuntimeError("OST directory contains no Ogg tracks")
            manifest["assets"]["music"] = {p.name: digest(p) for p in music.rglob("*.ogg")}
        if args.video:
            video_name = "big-buck-bunny-720p" + args.video.suffix.lower()
            shutil.copy2(args.video, data_root / "videos" / video_name)
            manifest["assets"][video_name] = digest(args.video)
        if wallpaper.is_file():
            shutil.copy2(wallpaper, data_root / "wallpaper.raw")
            manifest["assets"]["wallpaper.raw"] = digest(wallpaper)
        (data_root / "videos/ATTRIBUTION.md").write_text(
            "Big Buck Bunny — © 2008 Blender Foundation, CC BY 3.0.\n"
            "Source: https://download.blender.org/peach/bigbuckbunny_movies/\n"
            "License: https://creativecommons.org/licenses/by/3.0/\n")

        for name, tree, size, marker in [("nix", nix_root, args.nix_gib, "NIXSTORE"),
                                         ("data", data_root, args.data_gib, "BROOKDATA")]:
            image = output / f"{name}.img.partial"
            with image.open("xb") as file:
                file.truncate(size * 1024 ** 3)
            run("mkfs.ext2", "-q", "-F", "-b", "4096", "-L", marker,
                "-d", str(tree), str(image))
            if name == "data":
                for directory in ["/home", "/yq2", "/tmp"]:
                    for field, value in [("uid", "1000"), ("gid", "100")]:
                        run("debugfs", "-w", str(image), "-R",
                            f"set_inode_field {directory} {field} {value}")
            run("e2fsck", "-fn", str(image))
            image.rename(output / f"{name}.img")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Experiment disks and manifest: {output}")
    if not args.q2_dir and not args.bootstrap_mode:
        print("Quake 2 packages are staged; game data is not present yet.")


if __name__ == "__main__":
    main()
