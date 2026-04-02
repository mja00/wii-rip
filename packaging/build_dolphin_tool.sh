#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$repo_root/build/dolphin-tool/source"
build_dir="$repo_root/build/dolphin-tool/build"
output_path="$repo_root/build/dolphin-tool/dolphin-tool"
repo_url="https://github.com/dolphin-emu/dolphin.git"
ref="master"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

usage() {
  cat <<'EOF'
Usage: packaging/build_dolphin_tool.sh [options]

Clone Dolphin, build the standalone dolphin-tool target, and copy the resulting
binary to a known output path.

Options:
  --ref <git-ref>         Dolphin branch or tag to build (default: master)
  --repo-url <url>        Alternate Dolphin Git URL
  --source-dir <dir>      Clone directory (default: ./build/dolphin-tool/source)
  --build-dir <dir>       CMake build directory (default: ./build/dolphin-tool/build)
  --output <path>         Final dolphin-tool path (default: ./build/dolphin-tool/dolphin-tool)
  --jobs <count>          Parallel build jobs (default: detected CPU count)
  -h, --help              Show this help text
EOF
}

fail() {
  printf 'Error: %s\n' "$1" >&2
  exit 1
}

while (($# > 0)); do
  case "$1" in
    --ref)
      (($# >= 2)) || fail "missing value for --ref"
      ref="$2"
      shift 2
      ;;
    --repo-url)
      (($# >= 2)) || fail "missing value for --repo-url"
      repo_url="$2"
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
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

rm -rf "$source_dir" "$build_dir"
mkdir -p "$(dirname -- "$output_path")"

printf 'Cloning Dolphin (%s) from %s\n' "$ref" "$repo_url"
git clone --depth 1 --branch "$ref" "$repo_url" "$source_dir"

git \
  -c submodule."Externals/Qt".update=none \
  -c submodule."Externals/FFmpeg-bin".update=none \
  -c submodule."Externals/libadrenotools".update=none \
  -C "$source_dir" \
  submodule update --init --recursive --depth 1 --jobs "$jobs"

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_SYSTEM_LIBS=OFF \
  -DENABLE_CLI_TOOL=ON \
  -DENABLE_HEADLESS=ON \
  -DENABLE_QT=OFF \
  -DENABLE_NOGUI=OFF \
  -DENABLE_TESTS=OFF \
  -DENABLE_VULKAN=OFF \
  -DENABLE_X11=OFF \
  -DENABLE_EGL=OFF \
  -DENABLE_ALSA=OFF \
  -DENABLE_PULSEAUDIO=OFF \
  -DENABLE_CUBEB=OFF \
  -DENABLE_LLVM=OFF \
  -DENABLE_SDL=OFF \
  -DENABLE_AUTOUPDATE=OFF \
  -DENABLE_ANALYTICS=OFF \
  -DENCODE_FRAMEDUMPS=OFF \
  -DUSE_DISCORD_PRESENCE=OFF \
  -DUSE_MGBA=OFF \
  -DUSE_UPNP=OFF \
  -DENABLE_HWDB=OFF \
  -DENABLE_EVDEV=OFF

cmake --build "$build_dir" --target dolphin-tool --parallel "$jobs"

built_binary="$build_dir/Binaries/dolphin-tool"
[[ -f "$built_binary" ]] || fail "expected dolphin-tool build output at $built_binary"

cp "$built_binary" "$output_path"
chmod 755 "$output_path"

printf 'Built dolphin-tool:\n'
printf '  source: %s\n' "$source_dir"
printf '  output: %s\n' "$output_path"
