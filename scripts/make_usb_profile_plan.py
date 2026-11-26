#!/usr/bin/env python3
"""Sign an incremental USB-system profile plan for the RTX desktop.

The plan names a realised selected-desktop profile (for example the
nix/rtx-desktop-games.nix overlay of the SSD's current profile). Its bridge
cache holds only the store paths absent from the paths already on the SSD,
so applying it fetches just the additions. Run from the repo root inside
nix-shell (openssl and nix are needed):

  python3 scripts/make_usb_profile_plan.py --selection <store>/profile-selection.json \
    --present <store>/profile/closure.txt --profile <result> --sequence N \
    --launcher-id brook-quake2 ... --package name=<store path> ... \
    --key <plan key> --output <new directory>
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import stage_closure as stage  # noqa: E402
from usb_profile_plan import checked_plan  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--selection', type=Path, required=True, help="the SSD store's profile-selection.json")
    parser.add_argument('--present', type=Path, action='append', required=True,
                        help='file listing store paths already on the SSD (repeatable)')
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--sequence', type=int, required=True)
    parser.add_argument('--launcher-id', action='append', default=[], help='launcher added to the base set')
    parser.add_argument('--package', action='append', default=[], help='NAME=STORE_PATH row added to the receipt')
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('--public-key', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    stage.verify_private_key(args.key)
    selection = json.loads(args.selection.read_text())
    profile = args.profile.resolve()
    launchers = selection['manifest']['launcherIds'] + args.launcher_id
    exported = {entry.stem for entry in (profile / 'share/applications').glob('*.desktop')}
    if exported != set(launchers):
        raise SystemExit(f'profile desktop files {sorted(exported)} differ from launchers {launchers}')
    packages = list(selection['packages'])
    for row in args.package:
        name, path = row.split('=', 1)
        packages.append({'name': name, 'version': '', 'store': Path(path).resolve().name})
    present = set()
    for listing in args.present:
        present.update(Path(line.strip()).name for line in listing.read_text().splitlines() if line.strip())

    stage.upstream_has = lambda *_: False
    plan = stage.generate_plan(profile, (), None, False, packages, launcher_ids=launchers)
    plan['sequence'] = args.sequence
    args.output.mkdir(mode=0o700)
    (args.output / 'cache/nar').mkdir(parents=True)
    document = args.output / 'stage-plan.json'
    document.write_text(json.dumps(plan, sort_keys=True, separators=(',', ':')) + '\n')
    subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', args.key, '-in', document,
                    '-out', args.output / 'stage-plan.sig'], check=True)
    new = sorted(set(plan['closure']) - present)
    nar_bytes = 0
    for name in new:
        entry = plan['closure'][name]
        relative = 'nar/' + name[:32] + '.nar'
        with (args.output / 'cache' / relative).open('wb') as archive:
            subprocess.run([*stage.NIX, 'store', 'dump-path', str(stage.STORE / name)], check=True, stdout=archive)
        nar_bytes += entry['narSize']
        (args.output / 'cache' / (name[:32] + '.narinfo')).write_text(
            f"StorePath: /nix/store/{name}\nURL: {relative}\nCompression: none\n"
            f"NarHash: {entry['narHash']}\nNarSize: {entry['narSize']}\n"
            f"References: {' '.join(entry['references'])}\n")
    checked, digest, _ = checked_plan(args.output, args.public_key)
    summary = {'sequence': checked['sequence'], 'profile': checked['profile']['root'],
               'closure': len(checked['closure']), 'present': len(checked['closure']) - len(new),
               'new': new, 'new_nar_bytes': nar_bytes, 'launcher_ids': launchers,
               'space': checked['profile'].get('space'), 'plan_sha256': digest}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n')
    print(json.dumps({key: value for key, value in summary.items() if key != 'new'}))


if __name__ == '__main__':
    main()
