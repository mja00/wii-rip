#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
source_dir="$repo_root/build/wit/source"
output_path="$repo_root/build/wit/wit"
repo_url="https://github.com/Wiimm/wiimms-iso-tools.git"
ref="master"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
skip_if_present=0
force_reclone=0

usage() {
  cat <<'EOF'
Usage: packaging/build_wit.sh [options]

Clone Wiimms ISO Tools, build wit from source, and copy the resulting binary
to a known output path.

wit's upstream build has two quirks on macOS that this script works around:
  1. setup.sh uses gawk's gensub() extension. macOS ships BSD awk which
     lacks gensub, so the generated Makefile.setup is missing SYSTEM := mac.
     This script ensures Homebrew's gawk is earlier in PATH when invoking make.
  2. Apple Silicon's modern linker rejects misaligned atoms in dclib-numeric.o.
     XFLAGS=-Wl,-ld_classic switches to the classic linker, which tolerates the
     misalignment (future Xcode may remove -ld_classic and require a source fix).

Options:
  --ref <git-ref>         Branch, tag, or commit to build (default: master)
  --repo-url <url>        Alternate Wiimms ISO Tools Git URL
  --source-dir <dir>      Clone directory (default: ./build/wit/source)
  --output <path>         Final wit binary path (default: ./build/wit/wit)
  --jobs <count>          Parallel build jobs (default: detected CPU count)
  --force-reclone         Delete the source tree and re-clone before building
  --skip-if-present       Reuse an existing executable at --output if available
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
  printf 'Using cached wit:\n'
  printf '  output: %s\n' "$output_path"
  exit 0
fi

mkdir -p "$(dirname -- "$output_path")"

if ((force_reclone == 1)) || [[ ! -d "$source_dir" ]]; then
  rm -rf "$source_dir"
  printf 'Cloning wiimms-iso-tools (%s) from %s\n' "$ref" "$repo_url"
  git clone --depth 1 --branch "$ref" "$repo_url" "$source_dir"
else
  printf 'Using existing source tree: %s\n' "$source_dir"
fi

os_name="$(uname -s)"

# wit's setup.sh uses gawk's gensub() extension. On Linux, the default awk is
# typically gawk already. On macOS, the default awk is BSD awk which lacks
# gensub(), so we must make sure `awk` in PATH resolves to gawk before invoking
# make. This block:
#   1. Tests the current `awk` for gensub() support.
#   2. If that fails, scans a few known Homebrew gnubin locations and prepends
#      the first one that exists.
#   3. If that fails, checks if `gawk` is in PATH and creates a temporary shim
#      directory where `awk` is symlinked to gawk, then prepends it to PATH.
#   4. If everything fails, bails out with a clear error message.
awk_supports_gensub() {
  awk 'BEGIN { x = gensub(/a/, "b", "g", "a"); exit (x == "b" ? 0 : 1); }' \
    </dev/null >/dev/null 2>&1
}

if ! awk_supports_gensub; then
  for brew_prefix in /opt/homebrew /usr/local; do
    gnubin="$brew_prefix/opt/gawk/libexec/gnubin"
    if [[ -x "$gnubin/awk" ]]; then
      PATH="$gnubin:$PATH"
      export PATH
      break
    fi
  done
fi

if ! awk_supports_gensub; then
  if command -v gawk >/dev/null 2>&1; then
    awk_shim_dir="$(mktemp -d)"
    ln -sf "$(command -v gawk)" "$awk_shim_dir/awk"
    PATH="$awk_shim_dir:$PATH"
    export PATH
    # Best-effort cleanup: the shim dir is tiny and short-lived, and deleting
    # it eagerly would break subsequent build steps that still need `awk`.
  fi
fi

if ! awk_supports_gensub; then
  if [[ "$os_name" == "Darwin" ]]; then
    fail "wit's setup.sh requires gawk for its gensub() extension; install it with 'brew install gawk'"
  else
    fail "wit's setup.sh requires gawk for its gensub() extension; install the 'gawk' package"
  fi
fi

project_dir="$source_dir/project"
[[ -d "$project_dir" ]] || fail "expected Wiimms ISO Tools project directory at $project_dir"

# Re-run setup.sh from scratch so SYSTEM detection is fresh for this platform.
rm -f "$project_dir/Makefile.setup"

# setup.sh is tracked without the executable bit in the upstream repo after
# a shallow clone — invoke it through bash explicitly so we do not depend on
# filesystem permissions.
make_args=()
if [[ "$os_name" == "Darwin" ]]; then
  # Workaround: Apple Silicon's new ld rejects misaligned atoms in
  # dclib-numeric.o. -ld_classic falls back to the classic linker which
  # treats the misalignment as a warning only.
  make_args+=(XFLAGS="-Wl,-ld_classic")
fi

(
  cd "$project_dir"
  make clean >/dev/null 2>&1 || true
  make -j "$jobs" "${make_args[@]}" wit
)

built_binary="$project_dir/wit"
[[ -f "$built_binary" ]] || fail "expected wit build output at $built_binary"

cp "$built_binary" "$output_path"
chmod 755 "$output_path"

printf 'Built wit:\n'
printf '  source: %s\n' "$source_dir"
printf '  output: %s\n' "$output_path"
