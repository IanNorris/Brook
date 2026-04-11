#!/usr/bin/env python3
"""
Convert a PNG image to a C++ header with raw RGBA pixel data.

Usage:
    python3 bake_logo.py <input.png> <output.h> [max_height]

Resizes the image to fit within max_height pixels (default 200) while
preserving aspect ratio. Composites alpha onto a black background
(the kernel's default framebuffer colour).

Output: a C++ header with:
  - g_bootLogoWidth, g_bootLogoHeight
  - g_bootLogoData[] as uint32_t ARGB pixels (0xAARRGGBB)
"""

import sys
import struct
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install Pillow", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.h> [max_height]")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    max_height = int(sys.argv[3]) if len(sys.argv) > 3 else 200

    img = Image.open(input_path).convert("RGBA")
    print(f"Input: {img.width}x{img.height}")

    # Resize to fit within max_height
    if img.height > max_height:
        ratio = max_height / img.height
        new_w = int(img.width * ratio)
        new_h = max_height
        img = img.resize((new_w, new_h), Image.LANCZOS)
        print(f"Resized: {img.width}x{img.height}")

    w, h = img.width, img.height
    pixels = img.load()

    # Build pixel data as 0xAARRGGBB uint32_t values
    # Composite onto black background for non-opaque pixels
    data = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            # Pre-multiply alpha against black background
            if a < 255:
                r = (r * a) // 255
                g = (g * a) // 255
                b = (b * a) // 255
            # Store as 0x00RRGGBB (alpha pre-multiplied, top byte 0 for framebuffer)
            val = (r << 16) | (g << 8) | b
            data.append(val)

    # Write C++ header
    with open(output_path, "w") as f:
        f.write("// Auto-generated boot logo — do not edit.\n")
        f.write(f"// Source: {Path(input_path).name} ({w}x{h})\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"static constexpr uint32_t g_bootLogoWidth  = {w};\n")
        f.write(f"static constexpr uint32_t g_bootLogoHeight = {h};\n\n")

        # Write as a C array — 8 values per line for readability
        f.write(f"static const uint32_t g_bootLogoData[{w * h}] = {{\n")
        for i in range(0, len(data), 8):
            chunk = data[i:i+8]
            line = ", ".join(f"0x{v:08x}" for v in chunk)
            f.write(f"    {line},\n")
        f.write("};\n")

    total_kb = (w * h * 4) / 1024
    print(f"Output: {output_path} ({w}x{h}, {total_kb:.1f} KB)")


if __name__ == "__main__":
    main()
