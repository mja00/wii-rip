#!/usr/bin/env bash
#
# Regenerate Source/wii-banner-render/Externals/font/roboto_regular_ttf.h from
# the binary TTF in the same directory. Run after replacing the TTF (e.g.,
# updating to a newer Roboto release) so the embedded byte array stays in
# sync.
#
# Usage: packaging/regenerate_font_header.sh

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
font_dir="$repo_root/Source/wii-banner-render/Externals/font"
ttf_path="$font_dir/Roboto-Regular.ttf"
header_path="$font_dir/roboto_regular_ttf.h"

[[ -f "$ttf_path" ]] || {
  printf 'Error: TTF source missing at %s\n' "$ttf_path" >&2
  exit 1
}

python3 - "$ttf_path" "$header_path" <<'PY'
import sys

src, dst = sys.argv[1:3]

with open(src, "rb") as f:
    data = f.read()

with open(dst, "w") as out:
    out.write("// AUTO-GENERATED from Externals/font/Roboto-Regular.ttf\n")
    out.write("// Roboto Regular - Apache License 2.0 - Copyright 2015 Google Inc.\n")
    out.write("// See Externals/font/LICENSE.Roboto for the full license text.\n")
    out.write("// Do not edit by hand; regenerate with packaging/regenerate_font_header.sh\n\n")
    out.write("#ifndef WII_BANNER_RENDER_ROBOTO_REGULAR_TTF_H_\n")
    out.write("#define WII_BANNER_RENDER_ROBOTO_REGULAR_TTF_H_\n\n")
    out.write("#include <cstddef>\n")
    out.write("#include <cstdint>\n\n")
    out.write("namespace wii_banner_render {\n\n")
    out.write(f"// {len(data)} bytes\n")
    out.write("static const std::uint8_t kRobotoRegularTtf[] = {\n")
    line = []
    for b in data:
        line.append(f"0x{b:02x}")
        if len(line) == 16:
            out.write("    " + ",".join(line) + ",\n")
            line = []
    if line:
        out.write("    " + ",".join(line) + ",\n")
    out.write("};\n\n")
    out.write(f"static const std::size_t kRobotoRegularTtfLen = {len(data)};\n\n")
    out.write("}  // namespace wii_banner_render\n\n")
    out.write("#endif  // WII_BANNER_RENDER_ROBOTO_REGULAR_TTF_H_\n")

print(f"wrote {dst}: {len(data)} bytes of TTF data", file=sys.stderr)
PY
