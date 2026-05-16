#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: create_blend.sh <map_folder> [output.blend]" >&2
  exit 1
fi

map_folder=$1
output_blend=${2:-}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
script_parent=$(cd -- "$script_dir/.." && pwd)
current_dir=$(pwd)

find_blender() {
  if [[ -n "${BLENDER_PATH:-}" ]]; then
    printf '%s\n' "$BLENDER_PATH"
    return 0
  fi
  if [[ -f "$script_parent/blender_path.txt" ]]; then
    head -n 1 "$script_parent/blender_path.txt"
    return 0
  fi
  if [[ -f "$current_dir/blender_path.txt" ]]; then
    head -n 1 "$current_dir/blender_path.txt"
    return 0
  fi
  if command -v blender >/dev/null 2>&1; then
    printf '%s\n' "blender"
    return 0
  fi
  return 1
}

blender_exe=$(find_blender || true)
if [[ -z "$blender_exe" ]]; then
  echo "Could not find Blender." >&2
  echo "Set BLENDER_PATH or create blender_path.txt beside the app folder." >&2
  exit 1
fi

py_script="$script_parent/tools/create_blend.py"
if [[ ! -f "$py_script" ]]; then
  py_script="$current_dir/tools/create_blend.py"
fi
if [[ ! -f "$py_script" ]]; then
  echo "Could not find tools/create_blend.py relative to this script." >&2
  exit 1
fi

if [[ -n "$output_blend" ]]; then
  "$blender_exe" --factory-startup --background --python "$py_script" -- "$map_folder" "$output_blend"
else
  "$blender_exe" --factory-startup --background --python "$py_script" -- "$map_folder"
fi
