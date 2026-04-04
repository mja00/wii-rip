#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
binary_name="wii-rip"
output_root="$repo_root/dist"
package_name=""
create_tarball=0
skip_build=0
dolphin_tool_path=""
wit_path=""

usage() {
  cat <<'EOF'
Usage: packaging/build_release.sh [options]

Build the release binary and stage a portable release directory containing:
  - wii-rip
  - tools/dolphin-tool
  - tools/wit

Options:
  --out-dir <dir>        Output parent directory (default: ./dist)
  --name <name>          Package directory name (default: wii-rip-<os>-<arch>)
  --dolphin-tool <path>  Explicit path to dolphin-tool
  --wit <path>           Explicit path to wit
  --tar                  Create a .tar.gz alongside the staged directory
  --skip-build           Reuse an existing target/release/wii-rip binary
  -h, --help             Show this help text
EOF
}

fail() {
  printf 'Error: %s\n' "$1" >&2
  exit 1
}

resolve_tool() {
  local name="$1"
  local explicit_path="$2"

  if [[ -n "$explicit_path" ]]; then
    [[ -f "$explicit_path" ]] || fail "expected $name at $explicit_path, but no file exists there"
    [[ -x "$explicit_path" ]] || fail "expected $name at $explicit_path to be executable"
    printf '%s\n' "$explicit_path"
    return
  fi

  local discovered
  discovered="$(command -v "$name" || true)"
  [[ -n "$discovered" ]] || fail "could not find '$name' in PATH; pass --$name with an explicit path"
  printf '%s\n' "$discovered"
}

bundle_libs() {
  local binary="$1"
  local lib_dir="$2"

  # System libs that are guaranteed present on any Linux — skip these
  local skip_regex='linux-vdso\.so|ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread\.so|libgcc_s\.so|libstdc\+\+\.so|libz\.so'

  local lib_path
  ldd "$binary" | while read -r line; do
    lib_path="$(printf '%s\n' "$line" | grep -oP '=> \K/[^ ]+' || true)"
    [[ -n "$lib_path" ]] || continue
    if printf '%s\n' "$lib_path" | grep -qE "$skip_regex"; then
      continue
    fi
    mkdir -p "$lib_dir"
    cp -L "$lib_path" "$lib_dir/"
  done

  # Only set RPATH if we actually bundled any libs
  if compgen -G "$lib_dir/*.so*" > /dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/lib' "$binary"
  fi
}

default_package_name() {
  local os_name arch_name
  os_name="$(uname -s | tr '[:upper:]' '[:lower:]')"
  arch_name="$(uname -m | tr '[:upper:]' '[:lower:]')"
  printf '%s-%s-%s\n' "$binary_name" "$os_name" "$arch_name"
}

while (($# > 0)); do
  case "$1" in
    --out-dir)
      (($# >= 2)) || fail "missing value for --out-dir"
      output_root="$2"
      shift 2
      ;;
    --name)
      (($# >= 2)) || fail "missing value for --name"
      package_name="$2"
      shift 2
      ;;
    --dolphin-tool)
      (($# >= 2)) || fail "missing value for --dolphin-tool"
      dolphin_tool_path="$2"
      shift 2
      ;;
    --wit)
      (($# >= 2)) || fail "missing value for --wit"
      wit_path="$2"
      shift 2
      ;;
    --tar)
      create_tarball=1
      shift
      ;;
    --skip-build)
      skip_build=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

if [[ -z "$package_name" ]]; then
  package_name="$(default_package_name)"
fi

binary_path="$repo_root/target/release/$binary_name"
package_dir="$output_root/$package_name"
tools_dir="$package_dir/tools"

dolphin_tool_path="$(resolve_tool dolphin-tool "$dolphin_tool_path")"
wit_path="$(resolve_tool wit "$wit_path")"

if ((skip_build == 0)); then
  cargo build --release --manifest-path "$repo_root/Cargo.toml"
fi

[[ -f "$binary_path" ]] || fail "release binary not found at $binary_path"

rm -rf "$package_dir"
mkdir -p "$tools_dir"

cp "$binary_path" "$package_dir/$binary_name"
cp "$dolphin_tool_path" "$tools_dir/dolphin-tool"
cp "$wit_path" "$tools_dir/wit"
chmod 755 "$package_dir/$binary_name" "$tools_dir/dolphin-tool" "$tools_dir/wit"

bundle_libs "$tools_dir/wit" "$tools_dir/lib"
bundle_libs "$tools_dir/dolphin-tool" "$tools_dir/lib"

printf 'Packaged release directory:\n'
printf '  binary: %s\n' "$package_dir/$binary_name"
printf '  dolphin-tool: %s\n' "$tools_dir/dolphin-tool"
printf '  wit: %s\n' "$tools_dir/wit"

if ((create_tarball == 1)); then
  tarball_path="$output_root/$package_name.tar.gz"
  tar -C "$output_root" -czf "$tarball_path" "$package_name"
  printf '  tarball: %s\n' "$tarball_path"
fi
