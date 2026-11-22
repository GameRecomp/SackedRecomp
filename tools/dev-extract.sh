#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Development helper until `recomp extract` exists (M1): unpack a supported installer,
# ISO or zip into orig/<game>/<variant>/ with the installed directory layout.
#
#   tools/dev-extract.sh <media> <game> <variant> [--dest DIR]
#
# Needs: innoextract (GOG .exe), isoinfo + unshield (Gold ISO), unzip (Saga zip).
set -euo pipefail

usage() { echo "usage: $0 <media> <game> <variant> [--dest DIR]" >&2; exit 2; }
(( $# >= 3 )) || usage
media=$1 game=$2 variant=$3
shift 3
root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
dest="$root/orig/$game/$variant"
if (( $# )); then
  [[ $1 == --dest && $# == 2 ]] || usage
  dest=$2
fi
[[ -f "$media" ]] || { echo "not a file: $media" >&2; exit 1; }
need() { command -v "$1" >/dev/null || { echo "missing tool: $1" >&2; exit 1; }; }

if [[ -e "$dest" ]] && [[ -n "$(ls -A "$dest" 2>/dev/null)" ]]; then
  echo "destination not empty: $dest" >&2
  exit 1
fi
mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Move the contents of the single top-level directory in $1 (or $1 itself) to $dest.
move_tree() {
  local src=$1
  local entries=("$src"/*)
  if (( ${#entries[@]} == 1 )) && [[ -d "${entries[0]}" ]]; then
    src=${entries[0]}
  fi
  shopt -s dotglob
  mv "$src"/* "$dest"/
  shopt -u dotglob
}

case "${media,,}" in
  *.exe)
    need innoextract
    innoextract --quiet --output-dir "$tmp" --include app "$media"
    [[ -d "$tmp/app" ]] || { echo "no app/ directory in installer" >&2; exit 1; }
    move_tree "$tmp/app"
    ;;
  *.iso)
    need isoinfo
    need unshield
    mkdir -p "$tmp/cd" "$tmp/out"
    # Copy the InstallShield cabinets from the ISO root, then unpack them.
    while IFS= read -r p; do
      name=${p#/}
      isoinfo -R -i "$media" -x "$p" > "$tmp/cd/$name"
    done < <(isoinfo -R -f -i "$media" | grep -iE '^/[^/]+\.(cab|hdr)$')
    hdr=$(find "$tmp/cd" -maxdepth 1 -iname 'data1.hdr' | head -n 1)
    [[ -n "$hdr" ]] || { echo "no DATA1.HDR on the image" >&2; exit 1; }
    unshield -d "$tmp/out" x "$hdr" >/dev/null
    move_tree "$tmp/out"
    ;;
  *.zip)
    need unzip
    # Exit code 1 = warnings only (e.g. differing local/central names of "Münzsammler").
    rc=0
    unzip -q "$media" -d "$tmp/out" 2>"$tmp/unzip.log" || rc=$?
    if (( rc > 1 )); then cat "$tmp/unzip.log" >&2; exit 1; fi
    move_tree "$tmp/out"
    ;;
  *) echo "unsupported media type: $media" >&2; exit 1 ;;
esac

echo "extracted to $dest"
recomp="$root/recomp/target/release/recomp"
if [[ -x "$recomp" ]]; then
  "$recomp" identify --games "$root/games" "$dest" | tail -n 3
fi
