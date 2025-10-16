# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Please install Pillow: pip install pillow", file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) < 3:
        print("Usage: generate_logo_header.py <input.png> <output_header.h>")
        return 2

    in_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    im = Image.open(in_path).convert("RGBA")
    w, h = im.size
    data = im.tobytes()

    # Emit C header with RGBA data and size
    with open(out_path, "w", newline='\n', encoding="utf-8") as f:
        f.write("// Auto-generated from {}\n".format(in_path.name))
        f.write("#pragma once\n\n")
        f.write("#include <cstddef>\n\n")
        f.write("extern const unsigned int g_viewer_logo_width;\n")
        f.write("extern const unsigned int g_viewer_logo_height;\n")
        f.write("extern const unsigned char g_viewer_logo_rgba[];\n\n")

        f.write("#ifdef NANOVDB_EDITOR_DEFINE_LOGO_DATA\n")
        f.write("const unsigned int g_viewer_logo_width = {}u;\n".format(w))
        f.write("const unsigned int g_viewer_logo_height = {}u;\n".format(h))
        f.write("const unsigned char g_viewer_logo_rgba[] = {\n")
        # Write bytes as hex, 16 per line
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            line = ", ".join("0x{:02X}".format(b) for b in chunk)
            f.write("    " + line + ",\n")
        f.write("};\n")
        f.write("#endif // NANOVDB_EDITOR_DEFINE_LOGO_DATA\n")

    print("Wrote {} ({}x{})".format(out_path, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())

