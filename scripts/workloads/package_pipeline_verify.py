"""Verify only private benchmark outputs against the host's NAR inventory."""
import hashlib
import json
from pathlib import Path
import sys


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        name = str(path.relative_to(root))
        if path.is_symlink():
            result[name] = {'type': 'symlink', 'target': str(path.readlink())}
        elif path.is_dir():
            result[name] = {'type': 'directory'}
        else:
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            result[name] = {'type': 'file', 'bytes': path.stat().st_size, 'sha256': digest}
    return result


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


if __name__ == '__main__':
    root, round_number = Path(sys.argv[1]), int(sys.argv[2])
    expected = json.loads((root/'expected.json').read_text())
    for suffix in ('plain', 'pipe', 'zstd', 'network'):
        assert inventory(root/f'round-{round_number}-{suffix}') == expected['inventory'], suffix
    assert sha(root/f'round-{round_number}-copy.nar') == expected['nar_sha256']
    assert sha(root/f'round-{round_number}-download.zst') == expected['compressed_sha256']
    print(json.dumps({'pipeline_round': round_number, 'checksums_and_tree_pass': True}), flush=True)
