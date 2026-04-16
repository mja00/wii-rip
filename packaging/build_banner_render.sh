#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$repo_root/Source/wii-banner-render"
build_dir="$repo_root/build/wii-banner-render/build"
glew_dir="$repo_root/build/wii-banner-render/glew"
output_path="$repo_root/build/wii-banner-render/wii-banner-render"
glew_url="https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
skip_if_present=0

usage() {
  cat <<'EOF'
Usage: packaging/build_banner_render.sh [options]

Build the wii-banner-render headless banner-to-video renderer from the
vendored source tree at Source/wii-banner-render/.

The vendored tree is a modified copy of jordan-woyak/wii-banner-player; see
Source/wii-banner-render/MODIFICATIONS.md for the list of changes. GLEW is
downloaded into the build directory because it must be compiled with
GLEW_EGL for headless OpenGL.

Options:
  --build-dir <dir>       CMake build directory (default: ./build/wii-banner-render/build)
  --glew-dir <dir>        Directory holding GLEW source (default: ./build/wii-banner-render/glew)
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
    --glew-dir)
      (($# >= 2)) || fail "missing value for --glew-dir"
      glew_dir="$2"
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

[[ -f "$source_dir/CMakeLists.txt" ]] || \
  fail "vendored source tree missing at $source_dir"

mkdir -p "$(dirname -- "$output_path")"

# Download GLEW 2.2.0 if not already present. We compile GLEW ourselves with
# GLEW_EGL so glewInit() uses eglGetCurrentDisplay() instead of
# glXGetCurrentDisplay() — the system libGLEW.so is built in GLX mode and
# fails at runtime in headless environments.
if [[ ! -f "$glew_dir/src/glew.c" ]]; then
  printf 'Downloading GLEW 2.2.0 source...\n'
  rm -rf "$glew_dir"
  glew_tmp="$(mktemp -d)"
  curl -fsSL "$glew_url" | tar -xz -C "$glew_tmp"
  mkdir -p "$(dirname -- "$glew_dir")"
  mv "$glew_tmp/glew-2.2.0" "$glew_dir"
  rm -rf "$glew_tmp"
else
  printf 'Using existing GLEW source: %s\n' "$glew_dir"
fi

# Configure only when the build directory does not yet exist; subsequent runs
# reuse the existing Ninja build files for an incremental build.
if [[ ! -d "$build_dir" ]]; then
  cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGLEW_SOURCE_DIR="$glew_dir"
fi

cmake --build "$build_dir" --target wii-banner-render --parallel "$jobs"

built_binary="$build_dir/wii-banner-render"
[[ -f "$built_binary" ]] || fail "expected build output at $built_binary"

cp "$built_binary" "$output_path"
chmod 755 "$output_path"

printf 'Built wii-banner-render:\n'
printf '  source: %s\n' "$source_dir"
printf '  output: %s\n' "$output_path"
