#!/usr/bin/env python3
"""Collect freedesktop .desktop entries and convert icons for Brook OS launcher.

Scans Nix store closures for .desktop files, extracts metadata (Name, Exec,
Icon, Categories), finds matching icons from hicolor icon themes, converts
them to Brook's raw RGBA format, and outputs:

  - applications.idx: text manifest (one entry per line, tab-separated)
  - icons/<name>.rgba: raw 24x24 XRGB pixel data (no header, 24*24*4 bytes)

Usage:
    python3 collect_desktop_entries.py <nix_disk_mount> <output_dir>

The script scans <nix_disk_mount>/store/*/share/applications/*.desktop
and <nix_disk_mount>/store/*/share/icons/hicolor/ for matching icons.

Output manifest format (applications.idx):
    Name<TAB>Exec<TAB>IconFile<TAB>Categories
    Qalculate!<TAB>qalculate-gtk<TAB>qalculate.rgba<TAB>Utility;Calculator;
"""

import os
import sys
import struct
import configparser
import glob


ICON_SIZE = 24  # Target icon size in pixels


def parse_desktop_file(path):
    """Parse a .desktop file and return (Name, Exec, Icon, Categories) or None."""
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(path, encoding='utf-8')

    if not cp.has_section('Desktop Entry'):
        return None

    entry = cp['Desktop Entry']

    # Skip entries that are hidden or not applications
    if entry.get('Hidden', 'false').lower() == 'true':
        return None
    if entry.get('NoDisplay', 'false').lower() == 'true':
        return None
    entry_type = entry.get('Type', 'Application')
    if entry_type != 'Application':
        return None

    name = entry.get('Name', '').strip()
    exec_cmd = entry.get('Exec', '').strip()
    icon = entry.get('Icon', '').strip()
    categories = entry.get('Categories', '').strip()

    if not name or not exec_cmd:
        return None

    # Strip field codes from Exec (%f, %F, %u, %U, etc.)
    exec_parts = []
    for part in exec_cmd.split():
        if not part.startswith('%'):
            exec_parts.append(part)
    exec_cmd = ' '.join(exec_parts)

    return (name, exec_cmd, icon, categories)


def find_icon(store_root, icon_name, preferred_sizes=None):
    """Find an icon file in hicolor icon themes within the nix store.

    Searches for PNG icons at preferred sizes, falling back to any available size.
    Returns the path to the best match, or None.
    """
    if not icon_name:
        return None

    if preferred_sizes is None:
        preferred_sizes = [24, 32, 48, 22, 16, 64, 128, 256]

    # Search all store paths for icon themes
    icon_dirs = glob.glob(os.path.join(store_root, '*/share/icons/hicolor'))

    for size in preferred_sizes:
        size_dir = f"{size}x{size}"
        for icon_dir in icon_dirs:
            for category in ['apps', 'mimetypes', 'actions', 'categories']:
                candidates = [
                    os.path.join(icon_dir, size_dir, category, f"{icon_name}.png"),
                    os.path.join(icon_dir, size_dir, category, f"{icon_name}.svg"),
                ]
                for c in candidates:
                    if os.path.isfile(c) and c.endswith('.png'):
                        return c

    # Fallback: search pixmaps directories
    pixmap_dirs = glob.glob(os.path.join(store_root, '*/share/pixmaps'))
    for pdir in pixmap_dirs:
        for ext in ['.png', '.xpm']:
            candidate = os.path.join(pdir, f"{icon_name}{ext}")
            if os.path.isfile(candidate) and ext == '.png':
                return candidate

    # Fallback: any size in hicolor
    for icon_dir in icon_dirs:
        pattern = os.path.join(icon_dir, '*/apps', f"{icon_name}.png")
        matches = glob.glob(pattern)
        if matches:
            return matches[0]

    return None


def convert_icon_to_rgba(icon_path, output_path, size=ICON_SIZE):
    """Convert a PNG icon to raw XRGB pixels at the target size.

    Output format: size*size*4 bytes, each pixel is 0x00RRGGBB (little-endian uint32).
    No header — fixed size is known by the loader.
    """
    try:
        from PIL import Image
    except ImportError:
        print("WARNING: Pillow not installed, skipping icon conversion", file=sys.stderr)
        return False

    try:
        img = Image.open(icon_path).convert("RGBA")
        img = img.resize((size, size), Image.LANCZOS)

        with open(output_path, 'wb') as f:
            pixels = img.load()
            for y in range(size):
                for x in range(size):
                    r, g, b, a = pixels[x, y]
                    # Brook uses ARGB format in the framebuffer (alpha in high byte)
                    pixel = (a << 24) | (r << 16) | (g << 8) | b
                    f.write(struct.pack('<I', pixel))
        return True
    except Exception as e:
        print(f"WARNING: Failed to convert {icon_path}: {e}", file=sys.stderr)
        return False


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <nix_store_dir> <output_dir>")
        print()
        print("  nix_store_dir: Path containing nix store packages (e.g., mount/store)")
        print("  output_dir:    Output directory for applications.idx and icons/")
        sys.exit(1)

    store_dir = sys.argv[1]
    output_dir = sys.argv[2]

    if not os.path.isdir(store_dir):
        print(f"ERROR: Store directory not found: {store_dir}")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)
    icons_dir = os.path.join(output_dir, 'icons')
    os.makedirs(icons_dir, exist_ok=True)

    # Scan for .desktop files
    desktop_files = glob.glob(os.path.join(store_dir, '*/share/applications/*.desktop'))
    print(f"Found {len(desktop_files)} .desktop files")

    entries = []
    seen_names = set()

    for df in sorted(desktop_files):
        result = parse_desktop_file(df)
        if result is None:
            continue

        name, exec_cmd, icon_name, categories = result

        # Deduplicate by name
        if name in seen_names:
            continue

        # Validate: check that the binary actually exists in the store.
        # The first token of exec_cmd is the binary path or name.
        binary = exec_cmd.split()[0] if exec_cmd else ""
        binary_found = False

        if binary.startswith('/'):
            # Absolute path — check directly (relative to store root parent)
            # e.g. /nix/store/xxx/bin/vlc -> store_dir/../xxx/bin/vlc
            store_parent = os.path.dirname(store_dir)  # parent of "store/"
            abs_check = os.path.join(store_parent, binary.lstrip('/').replace('nix/', '', 1))
            if os.path.isfile(abs_check):
                binary_found = True
            # Also try the raw path under store_dir
            for sp in glob.glob(os.path.join(store_dir, '*/bin', os.path.basename(binary))):
                if os.path.isfile(sp):
                    binary_found = True
                    break
        else:
            # Bare command name — search all store packages' bin/ directories
            for sp in glob.glob(os.path.join(store_dir, '*/bin', binary)):
                if os.path.isfile(sp):
                    binary_found = True
                    break

        if not binary_found:
            print(f"  Skip: {name} [{binary}] — binary not found in store")
            continue

        seen_names.add(name)

        # Try to find and convert the icon
        icon_file = ""
        if icon_name:
            icon_path = find_icon(store_dir, icon_name)
            if icon_path:
                safe_name = icon_name.replace('/', '_').replace(' ', '_')
                rgba_filename = f"{safe_name}.rgba"
                rgba_path = os.path.join(icons_dir, rgba_filename)
                if convert_icon_to_rgba(icon_path, rgba_path):
                    icon_file = rgba_filename
                    print(f"  Icon: {icon_name} ({os.path.basename(icon_path)}) -> {rgba_filename}")

        entries.append((name, exec_cmd, icon_file, categories))
        print(f"  App: {name} [{exec_cmd}]")

    # Write manifest
    idx_path = os.path.join(output_dir, 'applications.idx')
    with open(idx_path, 'w') as f:
        for name, exec_cmd, icon_file, categories in sorted(entries):
            f.write(f"{name}\t{exec_cmd}\t{icon_file}\t{categories}\n")

    print(f"\nWrote {len(entries)} entries to {idx_path}")
    print(f"Icons directory: {icons_dir}")


if __name__ == '__main__':
    main()
