#!/usr/bin/env python3
"""Inventory exact runtime derivations, source inputs and installed notices.

This generates review evidence, not an automatic licence certification. A
release still needs human review of vendored/generated sources, copied assets
and each component's obligations. --archive saves available source inputs;
missing inputs remain explicit blockers rather than a fictitious source offer.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
STORE = re.compile(r'/nix/store/[a-z0-9]{32}-[^\s\"\';:<>]+')


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def read_derivations(drvs):
    if not drvs:
        return {}
    definitions = json.loads(output('nix', '--extra-experimental-features', 'nix-command', 'derivation', 'show', *drvs))
    if 'derivations' not in definitions:
        return definitions
    normalized = {}
    for name, definition in definitions['derivations'].items():
        definition['inputSrcs'] = ['/nix/store/' + item for item in definition.get('inputs', {}).get('srcs', [])]
        definition['inputDrvs'] = {'/nix/store/' + name: value for name, value in definition.get('inputs', {}).get('drvs', {}).items()}
        for result in definition.get('outputs', {}).values():
            if result.get('path') and not result['path'].startswith('/'):
                result['path'] = '/nix/store/' + result['path']
        normalized['/nix/store/' + name] = definition
    return normalized


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archive', action='store_true')
    parser.add_argument('--extra-derivation', action='append', default=[], help='Additional locally evaluated recipe; accepted only for matching exact output paths')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    closure = output('nix-store', '-qR', args.profile).splitlines()
    # Explicit additional inputs: firmware and fonts copied into artifacts,
    # plus the static installer's libc. They may not leave runtime references.
    extras_expr = '''let p = import ./nix/nixpkgs.nix {}; in map toString
      [p.OVMF.fd p.dejavu_fonts p.hack-font p.noto-fonts p.adwaita-icon-theme
       p.pkgsCross.musl64.stdenv.cc.libc]'''
    extras = json.loads(output('nix-instantiate', '--eval', '--strict', '--json', '--expr', extras_expr))
    paths = sorted(set(closure + extras))
    info = json.loads(output('nix', '--extra-experimental-features', 'nix-command', 'path-info', '--json', *paths))
    if isinstance(info, list):
        info = {item['path']: item for item in info}
    mapping = {path: item.get('deriver') or 'unknown-deriver' for path, item in info.items()}
    derivations = list(mapping.values())
    missing_drvs = sorted(set(d for d in derivations if d.endswith('.drv') and not Path(d).exists()))
    drvs = sorted(set(d for d in derivations if d.endswith('.drv') and Path(d).exists()))
    definitions = read_derivations(sorted(set(drvs + args.extra_derivation)))
    # Cache metadata can name a missing alternate derivation. Recover only an
    # available recipe whose declared output is the exact installed store path;
    # package-name/version similarity is deliberately insufficient.
    reported_mapping = dict(mapping)
    for depth in range(4):
        by_output = {result.get('path'): drv for drv, definition in definitions.items()
                     for result in definition.get('outputs', {}).values()}
        unmapped = []
        for path in paths:
            if mapping.get(path) not in definitions:
                if path in by_output:
                    mapping[path] = by_output[path]
                else:
                    unmapped.append(path)
        if not unmapped:
            break
        candidates = sorted({drv for definition in definitions.values() for drv in definition.get('inputDrvs', {})
                             if drv not in definitions and Path(drv).exists()})
        if not candidates:
            break
        definitions.update(read_derivations(candidates))
    source_files = {path for path in paths if mapping.get(path) not in definitions
                    and Path(path).is_file() and (Path(path).read_bytes()[:2] == b'#!' or Path(path).name.endswith('.sh'))}
    missing_drvs = sorted({mapping[path] for path in paths if mapping.get(path) not in definitions and path not in source_files})
    (args.output / 'derivations.json').write_text(json.dumps(definitions, indent=2) + '\n')
    recipe_tree = json.loads(output('nix-instantiate', '--eval', '--strict', '--json', '--expr', 'toString (import ./nix/nixpkgs.nix {}).path'))
    sources, source_refs, missing_notices = {recipe_tree} | source_files, {}, []
    notices_dir = args.output / 'notices'
    notices_dir.mkdir()
    for path in paths:
        root, found = Path(path), []
        # Limit to text-ish installed licensing documents; never traverse symlink
        # trees into unrelated store paths or copy application/user payload.
        for directory, dirs, files in os.walk(root, followlinks=False):
            for name in files:
                lower = name.lower()
                if lower.startswith(('license', 'licence', 'copying', 'copyright', 'notice')):
                    file = Path(directory) / name
                    if file.is_file() and file.stat().st_size <= 32 * 1024 * 1024:
                        target = notices_dir / root.name / file.relative_to(root)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(file, target)
                        found.append(str(file.relative_to(root)))
        if not found:
            missing_notices.append(path)
        drv = mapping.get(path)
        definition = definitions.get(drv, {})
        env = definition.get('env', {})
        env = dict(env, **definition.get('structuredAttrs', {}))
        if '__json' in env:
            env.update(json.loads(env['__json']))
        selected = set(definition.get('inputSrcs', []))
        # Source trees/archives, patches and vendored dependency outputs. Keep
        # exact references and recipes; do not substitute current upstream HEAD.
        for key, value in env.items():
            if key in ('src', 'srcs', 'patches', 'cargoDeps', 'npmDeps', 'goModules', 'vendor') or key.lower().endswith(('source', 'sources')):
                for match in STORE.findall(value if isinstance(value, str) else json.dumps(value)):
                    parts = Path(match).parts
                    selected.add('/' + '/'.join(parts[1:4]))
        source_refs[path] = dict(deriver=drv, cache_reported_deriver=reported_mapping.get(path),
                                source_inputs=sorted(selected), installed_notices=found)
        sources.update(selected)
    available, missing = [], []
    for source in sorted(sources):
        (available if Path(source).exists() else missing).append(source)
    if args.archive:
        # Stream rather than duplicate large Chromium/LLVM source trees. Bound
        # compressor concurrency so source collection does not swamp VM tests.
        with subprocess.Popen(['zstd', '-T2', '-3', '-o', str(args.output / 'available-source-inputs.tar.zst')], stdin=subprocess.PIPE) as compressor:
            with tarfile.open(fileobj=compressor.stdin, mode='w|', dereference=False) as tar:
                for index, source in enumerate(available, 1):
                    print(f'Archiving source input {index}/{len(available)}: {source}', flush=True)
                    tar.add(source, arcname='store/' + Path(source).name, recursive=True)
            compressor.stdin.close()
            if compressor.wait() != 0:
                raise RuntimeError('Source archive compression failed')
    report = dict(status='review-required', profile=args.profile, runtime_closure=closure,
                  copied_asset_inputs=extras, pinned_nixpkgs_recipes=recipe_tree, components=source_refs,
                  available_source_inputs=available, missing_source_inputs=missing,
                  packages_without_installed_notices=missing_notices, missing_derivations=missing_drvs,
                  outstanding=['Review generated/vendor inputs and copied dependency assets',
                               'Collect missing exact source inputs and component notices',
                               'Include matching Brook source, submodules and pinned Nixpkgs recipes',
                               'Review firmware, font, codec and artwork licences before publication'])
    (args.output / 'source-review.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(paths)} outputs, {len(available)} available source inputs, {len(missing)} missing source inputs; human review remains')


if __name__ == '__main__':
    main()
