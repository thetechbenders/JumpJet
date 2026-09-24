#!/usr/bin/env bash
# Print a directory holding dc_pid sources at the exact commit pinned in
# main/idf_component.yml, so host tests compile the dependency the firmware
# builds against. No second pin exists.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
manifest="$root/main/idf_component.yml"
field() {
    awk -v key="$1" '
        /^  dc_pid:[[:space:]]*$/ { inside=1; next }
        inside && /^  [^ ]/ { exit }
        inside && $1 == key":" { print $2; exit }
    ' "$manifest"
}
url=$(field git); path=$(field path); commit=$(field version)
[ -n "$url" ] && [ -n "$path" ] && [ -n "$commit" ] || {
    echo "dc_pid dependency missing from $manifest" >&2; exit 1; }
case "$commit" in
    *[!0-9a-f]*|"") echo "dc_pid pin must be a full commit SHA" >&2; exit 1 ;;
esac
[ "${#commit}" -eq 40 ] || { echo "dc_pid pin must be a full commit SHA" >&2; exit 1; }
dest="$root/build/host-deps/dc_pid-$commit"
if [ ! -f "$dest/dc_pid.c" ] || [ ! -f "$dest/include/dc_pid.h" ]; then
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    git -C "$tmp" init -q
    git -C "$tmp" fetch -q --depth 1 "$url" "$commit"
    [ "$(git -C "$tmp" rev-parse FETCH_HEAD)" = "$commit" ] || {
        echo "fetched dc_pid commit does not match pin" >&2; exit 1; }
    git -C "$tmp" checkout -q FETCH_HEAD -- "$path"
    mkdir -p "$dest"
    cp -R "$tmp/$path/." "$dest/"
fi
printf '%s\n' "$dest"
