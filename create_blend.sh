#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  create_blend.sh <map_folder> [output.blend]

Set BLENDER_PATH or put Blender's executable path in blender_path.txt.
EOF
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

map_folder=$1
output_blend=${2:-}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_dir=$(pwd)
blender_exe=${BLENDER_PATH:-}

if [[ -z $blender_exe && -f "$script_dir/blender_path.txt" ]]; then
    blender_exe=$(head -n 1 "$script_dir/blender_path.txt")
fi
if [[ -z $blender_exe && -f "$current_dir/blender_path.txt" ]]; then
    blender_exe=$(head -n 1 "$current_dir/blender_path.txt")
fi
if [[ -z $blender_exe ]] && command -v blender >/dev/null 2>&1; then
    blender_exe=blender
fi
if [[ -z $blender_exe ]]; then
    echo "Could not find Blender." >&2
    echo "Set BLENDER_PATH or create blender_path.txt beside this script." >&2
    exit 1
fi

py_script="$script_dir/tools/create_blend.py"
if [[ ! -f $py_script ]]; then
    py_script="$current_dir/tools/create_blend.py"
fi
if [[ ! -f $py_script ]]; then
    echo "Could not find tools/create_blend.py beside this script or in the current directory." >&2
    exit 1
fi

if [[ -z $output_blend ]]; then
    "$blender_exe" --factory-startup --background --python "$py_script" -- "$map_folder"
else
    "$blender_exe" --factory-startup --background --python "$py_script" -- "$map_folder" "$output_blend"
fi
