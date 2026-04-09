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
banner_render_path=""

usage() {
  cat <<'EOF'
Usage: packaging/build_release.sh [options]

Build the release binary and stage a portable release directory containing:
  - wii-rip
  - tools/dolphin-tool
  - tools/wit
  - tools/wii-banner-render  (optional; required for --video / --video-only)

Options:
  --out-dir <dir>              Output parent directory (default: ./dist)
  --name <name>                Package directory name (default: wii-rip-<os>-<arch>)
  --dolphin-tool <path>        Explicit path to dolphin-tool
  --wit <path>                 Explicit path to wit
  --banner-render <path>       Explicit path to wii-banner-render (optional)
  --tar                        Create a .tar.gz alongside the staged directory
  --skip-build                 Reuse an existing target/release/wii-rip binary
  -h, --help                   Show this help text
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

# Like resolve_tool but returns an empty string (instead of failing) when the
# tool is not found. Used for optional helpers like wii-banner-render.
resolve_optional_tool() {
  local name="$1"
  local explicit_path="$2"

  if [[ -n "$explicit_path" ]]; then
    [[ -f "$explicit_path" ]] || fail "expected $name at $explicit_path, but no file exists there"
    [[ -x "$explicit_path" ]] || fail "expected $name at $explicit_path to be executable"
    printf '%s\n' "$explicit_path"
    return
  fi

  command -v "$name" || true
}

bundle_libs_linux() {
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

# macOS equivalent of bundle_libs_linux using otool + install_name_tool.
#
# Walks the dependency graph of `binary` transitively. For every non-system
# dylib it encounters, it copies the file next to the binary (into $lib_dir),
# rewrites the referencing binary's install names to `@rpath/<name>`, rewrites
# the copied dylib's own LC_ID_DYLIB to `@rpath/<name>` so transitive rewrites
# match, and adds `@loader_path/lib` as an rpath on the top-level binary.
#
# System libraries shipped with macOS (/usr/lib/*, /System/*) are skipped
# because they are guaranteed to be present on any target mac. References
# that already use @rpath, @loader_path, or @executable_path are assumed to
# be self-contained and are left alone.
bundle_libs_macos() {
  local binary="$1"
  local lib_dir="$2"

  command -v otool >/dev/null 2>&1 \
    || fail "otool is required to bundle dylibs on macOS (install Xcode command line tools)"
  command -v install_name_tool >/dev/null 2>&1 \
    || fail "install_name_tool is required to bundle dylibs on macOS (install Xcode command line tools)"

  # Emit every non-system dylib reference from a Mach-O file, one per line.
  # Skips the first otool line (the file's own install name) and any
  # @rpath/@loader_path/@executable_path entries.
  _list_external_dylibs() {
    local target="$1"
    local line ref
    while IFS= read -r line; do
      ref="${line#"${line%%[![:space:]]*}"}"
      ref="${ref% (*}"
      [[ -n "$ref" ]] || continue
      case "$ref" in
        *":") continue ;;                         # "binary:" header line
        /usr/lib/*|/System/*) continue ;;         # macOS system libs
        @rpath/*|@loader_path/*|@executable_path/*) continue ;;
      esac
      printf '%s\n' "$ref"
    done < <(otool -L "$target" 2>/dev/null)
  }

  # macOS ships bash 3.2, which does not support associative arrays.
  # Track processed refs as newline-separated entries in a single string
  # so membership is a plain substring check against a delimited sentinel.
  local queue=()
  local processed=$'\n'

  # Seed the queue with the binary's direct external dependencies.
  local dep
  while IFS= read -r dep; do
    [[ -n "$dep" ]] && queue+=("$dep")
  done < <(_list_external_dylibs "$binary")

  local bundled_any=0
  while ((${#queue[@]} > 0)); do
    local ref="${queue[0]}"
    queue=("${queue[@]:1}")

    # Skip anything we have already seen. The newline-delimited substring
    # check is O(n) per lookup but the dep graphs we bundle are small.
    case "$processed" in
      *$'\n'"$ref"$'\n'*) continue ;;
    esac
    processed+="$ref"$'\n'

    if [[ ! -f "$ref" ]]; then
      printf 'warning: dylib referenced somewhere in %s not found on disk: %s\n' \
        "$binary" "$ref" >&2
      continue
    fi

    mkdir -p "$lib_dir"
    local base dest
    base="$(basename "$ref")"
    dest="$lib_dir/$base"

    local dep_is_new=0
    if [[ ! -f "$dest" ]]; then
      cp -L "$ref" "$dest"
      chmod u+w "$dest"
      install_name_tool -id "@rpath/$base" "$dest" 2>/dev/null || true
      dep_is_new=1
    fi

    # Rewrite the reference in the top-level binary (harmless if the binary
    # does not actually reference it — install_name_tool -change is a no-op
    # when the old name is not present).
    install_name_tool -change "$ref" "@rpath/$base" "$binary" 2>/dev/null || true

    # Also rewrite the reference in every already-bundled dylib that might
    # depend on this one. Since we copied each dep into $lib_dir, it is
    # sufficient to rewrite every *.dylib under $lib_dir. This is cheap and
    # ensures correct chains regardless of traversal order.
    local copied
    for copied in "$lib_dir"/*.dylib; do
      [[ -f "$copied" ]] || continue
      install_name_tool -change "$ref" "@rpath/$base" "$copied" 2>/dev/null || true
    done

    bundled_any=1

    # Walk transitively: enqueue every external dependency of the copy we
    # just made. Using the copy means the queue sees canonicalised paths.
    if ((dep_is_new == 1)); then
      local transitive
      while IFS= read -r transitive; do
        [[ -n "$transitive" ]] || continue
        case "$processed" in
          *$'\n'"$transitive"$'\n'*) continue ;;
        esac
        queue+=("$transitive")
      done < <(_list_external_dylibs "$dest")
    fi
  done

  if ((bundled_any == 1)); then
    # Add an rpath that resolves to the bundled lib dir regardless of cwd.
    # Tolerate "file already has LC_RPATH" errors if called repeatedly.
    install_name_tool -add_rpath '@loader_path/lib' "$binary" 2>/dev/null || true
  fi
}

bundle_libs() {
  local binary="$1"
  local lib_dir="$2"

  case "$(uname -s)" in
    Linux)
      bundle_libs_linux "$binary" "$lib_dir"
      ;;
    Darwin)
      bundle_libs_macos "$binary" "$lib_dir"
      ;;
    *)
      fail "unsupported platform for library bundling: $(uname -s)"
      ;;
  esac
}

default_package_name() {
  local os_raw os_name arch_name
  os_raw="$(uname -s | tr '[:upper:]' '[:lower:]')"
  arch_name="$(uname -m | tr '[:upper:]' '[:lower:]')"
  case "$os_raw" in
    darwin)
      os_name="macos"
      ;;
    *)
      os_name="$os_raw"
      ;;
  esac
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
    --banner-render)
      (($# >= 2)) || fail "missing value for --banner-render"
      banner_render_path="$2"
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
banner_render_path="$(resolve_optional_tool wii-banner-render "$banner_render_path")"

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

if [[ -n "$banner_render_path" ]]; then
  cp "$banner_render_path" "$tools_dir/wii-banner-render"
  chmod 755 "$tools_dir/wii-banner-render"
  bundle_libs "$tools_dir/wii-banner-render" "$tools_dir/lib"
fi

printf 'Packaged release directory:\n'
printf '  binary: %s\n' "$package_dir/$binary_name"
printf '  dolphin-tool: %s\n' "$tools_dir/dolphin-tool"
printf '  wit: %s\n' "$tools_dir/wit"
if [[ -n "$banner_render_path" ]]; then
  printf '  wii-banner-render: %s\n' "$tools_dir/wii-banner-render"
else
  printf '  wii-banner-render: not included (--video / --video-only will not work in this bundle)\n'
fi

if ((create_tarball == 1)); then
  tarball_path="$output_root/$package_name.tar.gz"
  tar -C "$output_root" -czf "$tarball_path" "$package_name"
  printf '  tarball: %s\n' "$tarball_path"
fi
