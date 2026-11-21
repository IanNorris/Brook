"""Build a managed catalog from the declared subset of a realized profile."""

from pathlib import Path
import sys
import tempfile

from collect_desktop_entries import collect

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT / 'tools/nix-install'))
from stage_launchers import desktop_command


def collect_selected(store, output, profile, launcher_ids, *, hardware=False):
    # Restrict the collector's view before it resolves duplicate titles and
    # converts icons. Filtering its result afterwards could retain the wrong
    # entry when an extra command-line package provides the same title.
    expected = set(launcher_ids)
    with tempfile.TemporaryDirectory(prefix='brook-launcher-view-') as path:
        view = Path(path)
        (view/'bin').symlink_to(profile/'bin')
        apps = view/'share/applications'
        apps.mkdir(parents=True)
        for launcher in sorted(expected):
            entry = profile/'share/applications'/f'{launcher}.desktop'
            if not entry.is_file():
                raise ValueError(f'Selected launcher does not exist: {launcher}')
            (apps/entry.name).symlink_to(entry)
        collect(store, output, profile=view, hardware=hardware, profile_only=True)
    index = output/'applications.idx'
    rows = [row.split('\t') for row in index.read_text().splitlines()]
    actual = {row[4] for row in rows}
    if actual != expected or len(rows) != len(expected):
        missing = ', '.join(sorted(expected - actual))
        raise ValueError(f'Could not create the exact selected launcher catalog. Missing: {missing}')
    for row in rows:
        command = desktop_command(row[1], store.parent, profile)
        if not command:
            raise ValueError(f'No eligible desktop-user command for selected launcher: {row[4]}')
        row[1] = command
    index.write_text(''.join('\t'.join(row) + '\n' for row in rows))
