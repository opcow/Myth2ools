#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: extract_assets.sh <tags_folder|plugin_file> <mesh_tag> [out_folder] [--ora] [--overwrite] [--animation-frame N|none]" >&2
  exit 1
fi

tags_folder=$1
mesh_tag=$2
shift 2

out_folder=$mesh_tag
if [[ $# -gt 0 && ${1:0:2} != "--" ]]; then
  out_folder=$1
  shift
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
script_parent=$(cd -- "$script_dir/.." && pwd)
current_dir=$(pwd)
bin_dir=""

if [[ ! -e "$tags_folder" ]]; then
  echo "Input tag source not found: $tags_folder" >&2
  exit 1
fi

try_bin_dir() {
  local dir=$1
  if [[ -z "$bin_dir" && -x "$dir/extract_map" && -x "$dir/export_mesh" && -x "$dir/export_water_mesh" && -x "$dir/export_map_objects" && -x "$dir/export_map_actions" ]]; then
    bin_dir=$dir
  fi
}

try_bin_dir "$script_parent/bin"
try_bin_dir "$current_dir/bin"
try_bin_dir "$script_parent/build-linux"
try_bin_dir "$script_parent/build/Release"
try_bin_dir "$script_parent/build"
try_bin_dir "$current_dir/build-linux"
try_bin_dir "$current_dir/build"
try_bin_dir "$script_parent"
try_bin_dir "$current_dir"

if [[ -z "$bin_dir" ]]; then
  echo "Could not find required tools under $script_parent/bin." >&2
  exit 1
fi

extract_args=()
model_args=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ora)
      extract_args+=("--ora")
      shift
      ;;
    --overwrite)
      model_args+=("--overwrite")
      shift
      ;;
    --animation-frame)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --animation-frame" >&2
        exit 1
      fi
      model_args+=("--animation-frame" "$2")
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

echo "Extracting map assets..."
"$bin_dir/extract_map" "$tags_folder" "$mesh_tag" --out "$out_folder" "${extract_args[@]}"
[[ -f "$out_folder/manifest.json" ]] || { echo "extract_map did not produce $out_folder/manifest.json" >&2; exit 1; }

echo "Exporting terrain mesh..."
"$bin_dir/export_mesh" "$out_folder"

echo "Exporting water mesh..."
"$bin_dir/export_water_mesh" "$out_folder"

echo "Exporting map objects..."
"$bin_dir/export_map_objects" "$tags_folder" "$out_folder" "$out_folder/assets/terrain/displacement.obj" "${model_args[@]}"

echo "Exporting action scripts..."
"$bin_dir/export_map_actions" "$out_folder"

echo "Done."
