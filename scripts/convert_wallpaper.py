#!/usr/bin/env python3
"""Convert a JPEG/PNG wallpaper to raw XRGB pixel data for Brook OS.

Usage: python3 convert_wallpaper.py <input.jpg> <output.raw> [width] [height]

Default output size: 1920x1080. The image is resized and center-cropped to fit.
Output format: raw 32-bit XRGB pixels (0x00RRGGBB), row-major, no header.
"""

import sys
import struct

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input> <output.raw> [width] [height]")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    target_w = int(sys.argv[3]) if len(sys.argv) > 3 else 1920
    target_h = int(sys.argv[4]) if len(sys.argv) > 4 else 1080

    try:
        from PIL import Image
    except ImportError:
        print("ERROR: Pillow not installed. Install with: pip install Pillow")
        sys.exit(1)

    img = Image.open(input_path).convert("RGB")
    orig_w, orig_h = img.size
    print(f"Input: {orig_w}x{orig_h}")

    # Resize to cover target dimensions, then center-crop
    scale = max(target_w / orig_w, target_h / orig_h)
    new_w = int(orig_w * scale)
    new_h = int(orig_h * scale)
    img = img.resize((new_w, new_h), Image.LANCZOS)

    # Center crop
    left = (new_w - target_w) // 2
    top = (new_h - target_h) // 2
    img = img.crop((left, top, left + target_w, top + target_h))

    print(f"Output: {target_w}x{target_h} ({target_w * target_h * 4} bytes)")

    # Write raw XRGB pixels
    with open(output_path, "wb") as f:
        for y in range(target_h):
            for x in range(target_w):
                r, g, b = img.getpixel((x, y))
                f.write(struct.pack("<I", (r << 16) | (g << 8) | b))

    print(f"Written to {output_path}")

if __name__ == "__main__":
    main()
