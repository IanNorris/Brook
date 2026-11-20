#!/usr/bin/env python3
"""
bake_icons.py — rasterise the Clearwater line icons into a coverage atlas.

Usage:
    python3 bake_icons.py <size_px> <output.cpp> <symbol>

The icons are DESIGN.md's set (docs/design/clearwater-next/index.html, ICON):
a 16-unit grid drawn with a 1.5-unit round stroke. The output is a
brook::FontAtlas whose "glyphs" are the icons, one size_px square each, from
character code 'A' in the order of brook::ChromeGlyph
(src/kernel/src/chrome_display_list.h). The chrome executor tints them like
text, so they need no primitive of their own.
"""

import io
import os
import sys

import cairosvg
from PIL import Image

FIRST_CHAR = ord('A')
SPEAKER = 'M2.5 6.5h2l3-2.500v8l-3-2.500h-2z'
NET = 'M5 13V3m0 0L2.500 5.500M5 3l2.500 2.500M11 3v10m0 0-2.500-2.500M11 13l2.500-2.500'
# Order must match ChromeGlyph.
ICONS = [
    ('minimise', '<path d="M4 8h8"/>'),
    ('maximise', '<rect x="4" y="4" width="8" height="8" rx="1.5"/>'),
    ('restore', '<rect x="3.5" y="5.5" width="7" height="7" rx="1.5"/>'
                '<path d="M6 5.500v-1a1 1 0 0 1 1-1h4.500a1 1 0 0 1 1 1V9a1 1 0 0 1-1 1h-1"/>'),
    ('close', '<path d="m4.500 4.500 7 7m0-7-7 7"/>'),
    ('network', f'<path d="{NET}"/>'),
    ('network-off', f'<path d="{NET}" stroke-opacity="0.45"/><path d="M9.500 9.500l4 4m0-4-4 4"/>'),
    ('volume-high', f'<path d="{SPEAKER}M10 6a3 3 0 0 1 0 4m1.800-5.800a5.500 5.500 0 0 1 0 7.600"/>'),
    ('volume-low', f'<path d="{SPEAKER}M10 6a3 3 0 0 1 0 4"/>'),
    ('volume-muted', f'<path d="{SPEAKER}m8 0 3 3m0-3-3 3"/>'),
    ('temperature', '<path d="M8 2.500A1.500 1.500 0 0 0 6.500 4v5.200a2.800 2.800 0 1 0 3 0V4A1.500 1.500 0 0 0 8 2.500zM8 7v4.500"/>'),
    ('power', '<path d="M8 2.500v5M4.600 4.800a5 5 0 1 0 6.800 0"/>'),
    ('restart', '<path d="M12.500 8a4.500 4.500 0 1 1-1.300-3.200M12.500 2.600v2.600H9.900"/>'),
    ('bolt', '<path d="M9 2 4 9h3.500L7 14l5-7H8.500z"/>'),
    ('chevron-right', '<path d="m6 4 4 4-4 4"/>'),
    ('chevron-up', '<path d="m4 10 4-4 4 4"/>'),
    ('chevron-down', '<path d="m4 6 4 4 4-4"/>'),
    # Launcher (DESIGN.md "Launcher organisation"): search, the rail and the
    # category icons, the app menu.
    ('search', '<circle cx="7" cy="7" r="4.5"/><path d="M10.5 10.5 14 14"/>'),
    ('grid', '<rect x="3" y="3" width="4" height="4" rx="1"/><rect x="9" y="3" width="4" height="4" rx="1"/>'
             '<rect x="3" y="9" width="4" height="4" rx="1"/><rect x="9" y="9" width="4" height="4" rx="1"/>'),
    ('inbox', '<path d="m2.5 9 1.800-5h7.400l1.800 5v3.500h-11zm0 0h3.200l.8 1.500h3l.8-1.500h3.200"/>'),
    ('pin', '<path d="M6 2.500h4l-.5 4 2 2.500h-7l2-2.500zM8 9v4.500"/>'),
    ('trash', '<path d="M3.500 4.500h9m-6 0V3h3v1.500m-5 0 .6 8.500h5.800l.6-8.500"/>'),
    ('folder', '<path d="M2.5 4.5a1 1 0 0 1 1-1h2.8l1.2 1.5h5a1 1 0 0 1 1 1V12a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1z"/>'),
    ('sliders', '<path d="M2.5 5h4m3.2 0h3.8M2.5 11h3.8m3.2 0h4"/><circle cx="8.1" cy="5" r="1.6"/><circle cx="7.9" cy="11" r="1.6"/>'),
    ('globe', '<circle cx="8" cy="8" r="5.5"/><path d="M2.5 8h11M8 2.5c-2.2 2-2.2 9 0 11m0-11c2.2 2 2.2 9 0 11"/>'),
    ('play', '<path d="M5.5 3.5v9l7-4.5z"/>'),
    ('doc', '<path d="M4.5 2.5h7v11h-7zM6.5 5.5h3m-3 2.5h3m-3 2.5h2"/>'),
    ('brush', '<path d="m10.5 2.5 3 3-7 7H3.5v-3z"/>'),
    ('pad', '<rect x="2" y="5" width="12" height="7" rx="3"/><path d="M5 8.5h2M6 7.5v2m4.8-1h.1"/>'),
    ('spark', '<path d="M8 2.5v3m0 5v3M2.5 8h3m5 0h3"/>'),
    # Side dock (DESIGN.md "Side dock"): move a window into the sidebar on
    # either edge, and move the sidebar to the other edge.
    ('dock-left', '<rect x="2.5" y="3.5" width="11" height="9" rx="1.5"/><path d="M6.5 3.5v9"/>'),
    ('dock-right', '<rect x="2.5" y="3.5" width="11" height="9" rx="1.5"/><path d="M9.5 3.5v9"/>'),
    ('swap', '<path d="M3 5.5h10m0 0-2.5-2.5M13 5.5 10.5 8M13 10.5H3m0 0 2.5-2.5M3 10.5 5.5 13"/>'),
]
COLUMNS = 8
PAD = 1


def coverage(body, size):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="{size}" height="{size}" '
           'fill="none" stroke="#fff" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">'
           f'{body}</svg>')
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size, output_height=size)
    with Image.open(io.BytesIO(png)) as image:
        return image.convert('RGBA').getchannel('A').tobytes()


def main():
    if len(sys.argv) != 4:
        print(f'Usage: {sys.argv[0]} <size_px> <output.cpp> <symbol>', file=sys.stderr)
        sys.exit(1)
    size, out_cpp, symbol = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    if not symbol.isidentifier() or not symbol.isascii():
        raise SystemExit('Invalid C++ icon atlas symbol')
    rows = (len(ICONS) + COLUMNS - 1) // COLUMNS
    width = COLUMNS * (size + PAD) + PAD
    height = (rows * (size + PAD) + PAD + 7) & ~7
    atlas = bytearray(width * height)
    cells = []
    for index, (_, body) in enumerate(ICONS):
        x = PAD + (index % COLUMNS) * (size + PAD)
        y = PAD + (index // COLUMNS) * (size + PAD)
        pixels = coverage(body, size)
        for row in range(size):
            atlas[(y + row) * width + x:(y + row) * width + x + size] = pixels[row * size:(row + 1) * size]
        cells.append((x, y))
    os.makedirs(os.path.dirname(out_cpp) or '.', exist_ok=True)
    with open(out_cpp, 'w') as f:
        f.write('// AUTO-GENERATED by scripts/bake_icons.py — do not edit.\n')
        f.write(f'// Clearwater line icons, {size}px, codes from {chr(FIRST_CHAR)!r}.\n\n')
        f.write('#include "font_atlas.h"\n\nnamespace brook {\n\n')
        f.write(f'static const uint8_t s_atlasPixels[{len(atlas)}] = {{\n')
        for i in range(0, len(atlas), 16):
            f.write('    ' + ','.join(f'0x{b:02x}' for b in atlas[i:i + 16]) + ',\n')
        f.write('};\n\n')
        f.write(f'static const GlyphInfo s_glyphs[{len(ICONS)}] = {{\n')
        for (name, _), (x, y) in zip(ICONS, cells):
            f.write(f'    {{{x},{y},{x + size},{y + size},0,{size},{size}}},  // {name}\n')
        f.write('};\n\n')
        f.write(f'extern const FontAtlas {symbol} = {{\n    s_atlasPixels,\n    {width},\n    {height},\n'
                f'    s_glyphs,\n    {len(ICONS)},\n    {FIRST_CHAR},\n    {size},\n    0,\n    {size},\n}};\n\n')
        f.write('} // namespace brook\n')
    print(f'bake_icons: {len(ICONS)} icons at {size}px → {width}×{height} atlas')


if __name__ == '__main__':
    main()
