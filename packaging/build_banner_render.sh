#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$repo_root/Source/wii-banner-render"
build_dir="$repo_root/build/wii-banner-render/build"
output_path="$repo_root/build/wii-banner-render/wii-banner-render"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
skip_if_present=0

usage() {
  cat <<'EOF'
Usage: packaging/build_banner_render.sh [options]

Build the wii-banner-render helper from the vendored sources at
Source/wii-banner-render/. The sources (a fork of wii-banner-player plus a
trimmed slice of Dolphin and GLEW 2.2.0) live in-tree, so this script does
no cloning, downloading, or patching — it simply runs CMake/Ninja.

See Source/wii-banner-render/NOTICE.md for upstream attribution and the
list of in-repo modifications.

Options:
  --build-dir <dir>       CMake build directory (default: ./build/wii-banner-render/build)
  --output <path>         Final binary path (default: ./build/wii-banner-render/wii-banner-render)
  --jobs <count>          Parallel build jobs (default: detected CPU count)
  --skip-if-present       Reuse an existing executable at --output if available (for CI caching)
  -h, --help              Show this help text
EOF
}

fail() {
  printf 'Error: %s\n' "$1" >&2
  exit 1
}

while (($# > 0)); do
  case "$1" in
    --build-dir)
      (($# >= 2)) || fail "missing value for --build-dir"
      build_dir="$2"
      shift 2
      ;;
    --output)
      (($# >= 2)) || fail "missing value for --output"
      output_path="$2"
      shift 2
      ;;
    --jobs)
      (($# >= 2)) || fail "missing value for --jobs"
      jobs="$2"
      shift 2
      ;;
    --skip-if-present)
      skip_if_present=1
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

if ((skip_if_present == 1)) && [[ -x "$output_path" ]]; then
  printf 'Using cached wii-banner-render:\n'
  printf '  output: %s\n' "$output_path"
  exit 0
fi

[[ -f "$source_dir/CMakeLists.txt" ]] \
  || fail "vendored source tree missing at $source_dir"

mkdir -p "$(dirname -- "$output_path")"

# Configure only when the build directory does not yet exist; subsequent runs
# reuse the existing Ninja build files for an incremental build.
if [[ ! -d "$build_dir" ]]; then
  cmake_args=(
    -S "$source_dir"
    -B "$build_dir"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
  )

  case "$(uname -s)" in
    Darwin)
      # Pin to the host architecture so we never accidentally try to build a
      # universal binary (the vendored GLEW source is built per-arch).
      cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=$(uname -m)")
      # Target the running macOS version, mirroring build_dolphin_tool.sh.
      if command -v sw_vers >/dev/null 2>&1; then
        macos_version="$(sw_vers -productVersion | cut -d. -f1,2)"
        cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=$macos_version")
      fi
      ;;
  esac

  cmake "${cmake_args[@]}"
fi

cmake --build "$build_dir" --target wii-banner-render --parallel "$jobs"

built_binary="$build_dir/wii-banner-render"
[[ -f "$built_binary" ]] || fail "expected build output at $built_binary"

cp "$built_binary" "$output_path"
chmod 755 "$output_path"

printf 'Built wii-banner-render:\n'
printf '  source: %s\n' "$source_dir"
printf '  output: %s\n' "$output_path"
