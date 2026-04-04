#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$repo_root/build/wii-banner-render/source"
build_dir="$repo_root/build/wii-banner-render/build"
output_path="$repo_root/build/wii-banner-render/wii-banner-render"
repo_url="https://github.com/jordan-woyak/wii-banner-player.git"
ref="master"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
skip_if_present=0
force_reclone=0

usage() {
  cat <<'EOF'
Usage: packaging/build_banner_render.sh [options]

Clone wii-banner-player, apply wii-banner-render overlay files, and build a
headless banner-to-video renderer binary.

If the source tree already exists the clone step is skipped and the overlay
is re-applied before an incremental CMake build. This makes the script fast
for local iteration when editing the overlay files in Source/wii-banner-render/.

Options:
  --repo-url <url>        Alternate wii-banner-player Git URL
  --ref <git-ref>         Branch or tag to clone (default: master)
  --source-dir <dir>      Clone directory (default: ./build/wii-banner-render/source)
  --build-dir <dir>       CMake build directory (default: ./build/wii-banner-render/build)
  --output <path>         Final binary path (default: ./build/wii-banner-render/wii-banner-render)
  --jobs <count>          Parallel build jobs (default: detected CPU count)
  --force-reclone         Delete the source tree and re-clone before building
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
    --repo-url)
      (($# >= 2)) || fail "missing value for --repo-url"
      repo_url="$2"
      shift 2
      ;;
    --ref)
      (($# >= 2)) || fail "missing value for --ref"
      ref="$2"
      shift 2
      ;;
    --source-dir)
      (($# >= 2)) || fail "missing value for --source-dir"
      source_dir="$2"
      shift 2
      ;;
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
    --force-reclone)
      force_reclone=1
      shift
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

mkdir -p "$(dirname -- "$output_path")"

# Clone only when the source tree is absent or --force-reclone was requested.
if ((force_reclone == 1)) || [[ ! -d "$source_dir" ]]; then
  rm -rf "$source_dir" "$build_dir"
  printf 'Cloning wii-banner-player (%s) from %s\n' "$ref" "$repo_url"
  git clone --depth 1 --branch "$ref" "$repo_url" "$source_dir"
else
  printf 'Using existing source tree: %s\n' "$source_dir"
fi

# Always re-apply the overlay so local edits to Source/wii-banner-render/
# are picked up without a full reclone.
overlay_dir="$repo_root/Source/wii-banner-render"
printf 'Applying wii-banner-render overlay from %s\n' "$overlay_dir"
cp "$overlay_dir/CMakeLists.txt" "$source_dir/CMakeLists.txt"
cp "$overlay_dir/Main.cpp"       "$source_dir/Source/Main.cpp"
cp "$overlay_dir/Layout.h"       "$source_dir/Source/Layout.h"
cp "$overlay_dir/Sound.cpp"      "$source_dir/Source/Sound.cpp"

# Download GLEW 2.2.0 source if not already present.
# We compile GLEW ourselves with GLEW_EGL so that glewInit() uses
# eglGetCurrentDisplay() instead of glXGetCurrentDisplay().  The system
# libGLEW.so is built in GLX mode and fails at runtime in headless environments.
glew_dir="$source_dir/Externals/glew"
if [[ ! -d "$glew_dir" ]]; then
  printf 'Downloading GLEW 2.2.0 source...\n'
  glew_tmp="$(mktemp -d)"
  curl -fsSL \
    "https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.tgz" \
    | tar -xz -C "$glew_tmp"
  mv "$glew_tmp/glew-2.2.0" "$glew_dir"
  rm -rf "$glew_tmp"
else
  printf 'Using existing GLEW source: %s\n' "$glew_dir"
fi

# Configure only when the build directory does not yet exist; subsequent runs
# reuse the existing Ninja build files for an incremental build.
if [[ ! -d "$build_dir" ]]; then
  cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
fi

cmake --build "$build_dir" --target wii-banner-render --parallel "$jobs"

built_binary="$build_dir/wii-banner-render"
[[ -f "$built_binary" ]] || fail "expected build output at $built_binary"

cp "$built_binary" "$output_path"
chmod 755 "$output_path"

printf 'Built wii-banner-render:\n'
printf '  source: %s\n' "$source_dir"
printf '  output: %s\n' "$output_path"
