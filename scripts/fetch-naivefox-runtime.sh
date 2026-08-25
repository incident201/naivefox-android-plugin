#!/usr/bin/env bash

set -euo pipefail

if (( $# != 1 )); then
  printf 'usage: %s DESTINATION\n' "$0" >&2
  exit 2
fi

destination=$1
script_dir=$(cd "$(dirname "$0")" && pwd)
if [[ -e $destination ]] && [[ -n $(find "$destination" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]]; then
  printf 'destination must be absent or empty: %s\n' "$destination" >&2
  exit 2
fi
mkdir -p "$destination/download" "$destination/extracted"
destination=$(realpath "$destination")

release_json="$destination/release.json"
gh api -H 'Accept: application/vnd.github+json' \
  repos/incident201/naivefox/releases/latest >"$release_json"

if [[ $(jq -r '.draft' "$release_json") != false ]] ||
   [[ $(jq -r '.prerelease' "$release_json") != false ]]; then
  printf 'GitHub latest release is draft or prerelease\n' >&2
  exit 1
fi

release_tag=$(jq -r '.tag_name' "$release_json")
if [[ ! $release_tag =~ ^[0-9A-Za-z._-]+$ ]]; then
  printf 'unsafe latest release tag: %s\n' "$release_tag" >&2
  exit 1
fi

mapfile -t archives < <(
  jq -r '.assets[] | select(.name | test("-android-aarch64\\.tar\\.xz$")) | [.id, .name] | @tsv' \
    "$release_json"
)
if (( ${#archives[@]} != 1 )); then
  printf 'expected exactly one Android AArch64 archive, found %s\n' "${#archives[@]}" >&2
  exit 1
fi
IFS=$'\t' read -r archive_id archive_name <<<"${archives[0]}"
checksum_name="${archive_name}.sha256"
mapfile -t checksums < <(
  jq -r --arg name "$checksum_name" \
    '.assets[] | select(.name == $name) | [.id, .name] | @tsv' "$release_json"
)
if (( ${#checksums[@]} != 1 )); then
  printf 'expected checksum asset %s exactly once\n' "$checksum_name" >&2
  exit 1
fi
IFS=$'\t' read -r checksum_id downloaded_checksum_name <<<"${checksums[0]}"
[[ $downloaded_checksum_name == "$checksum_name" ]]

archive="$destination/download/$archive_name"
checksum="$destination/download/$checksum_name"
gh api -H 'Accept: application/octet-stream' \
  "repos/incident201/naivefox/releases/assets/$archive_id" >"$archive"
gh api -H 'Accept: application/octet-stream' \
  "repos/incident201/naivefox/releases/assets/$checksum_id" >"$checksum"

expected=$(awk 'NR == 1 { print tolower($1) }' "$checksum")
if [[ ! $expected =~ ^[0-9a-f]{64}$ ]]; then
  printf 'invalid SHA-256 file for %s\n' "$archive_name" >&2
  exit 1
fi
actual=$(sha256sum "$archive" | awk '{ print tolower($1) }')
if [[ $actual != "$expected" ]]; then
  printf 'SHA-256 mismatch for %s\n' "$archive_name" >&2
  exit 1
fi
printf 'Verified archive SHA-256: %s\n' "$actual"

python3 - "$archive" "$destination/extracted" <<'PY'
import sys
import tarfile
from pathlib import Path, PurePosixPath

archive = Path(sys.argv[1])
destination = Path(sys.argv[2])
top_levels = set()
with tarfile.open(archive, mode="r:xz") as package:
    members = package.getmembers()
    if not members:
        raise SystemExit("NaiveFox archive is empty")
    for member in members:
        name = member.name.rstrip("/")
        path = PurePosixPath(name)
        if not name or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
            raise SystemExit(f"unsafe archive path: {member.name!r}")
        if not member.isfile() and not member.isdir():
            raise SystemExit(f"archive contains a link or special file: {member.name!r}")
        top_levels.add(path.parts[0])
    if len(top_levels) != 1:
        raise SystemExit(f"archive must contain one top-level directory, got {sorted(top_levels)}")
    package.extractall(destination, filter="data")
print(next(iter(top_levels)))
PY

mapfile -t roots < <(find "$destination/extracted" -mindepth 1 -maxdepth 1 -type d -print)
if (( ${#roots[@]} != 1 )); then
  printf 'archive extraction did not produce exactly one package root\n' >&2
  exit 1
fi
runtime_root=$(realpath "${roots[0]}")
verification_json=$(python3 "$script_dir/verify_runtime.py" --json "$runtime_root")
runtime_version=$(jq -r '.version' <<<"$verification_json")
runtime_path=$(jq -r '.runtime_path' <<<"$verification_json")

if [[ -n ${GITHUB_OUTPUT:-} ]]; then
  {
    printf 'release_tag=%s\n' "$release_tag"
    printf 'archive_name=%s\n' "$archive_name"
    printf 'archive_sha256=%s\n' "$actual"
    printf 'runtime_root=%s\n' "$runtime_root"
    printf 'runtime_version=%s\n' "$runtime_version"
    printf 'runtime_path=%s\n' "$runtime_path"
  } >>"$GITHUB_OUTPUT"
fi

printf 'Latest NaiveFox release: %s\n' "$release_tag"
printf 'Verified runtime root: %s\n' "$runtime_root"
