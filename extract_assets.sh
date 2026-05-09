#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  extract_assets.sh <tags_folder> <meshtag> [output_folder] [--ora] [--overwrite] [--animation-frame first|none|all]

Runs extract_map, export_mesh, export_water_mesh, and export_models.
EOF
}

if [[ $# -lt 2 ]]; then
    usage
    exit 1
fi

tags_folder=$1
mesh_tag=$2
shift 2

out_folder=$mesh_tag
if [[ $# -gt 0 && $1 != --* ]]; then
    out_folder=$1
    shift
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
                usage
                exit 1
            fi
            model_args+=("--animation-frame" "$2")
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_dir=$(pwd)
bin_dir=

try_bin_dir() {
    local dir=$1
    if [[ -n $bin_dir ]]; then
        return
    fi
    if [[ -x "$dir/extract_map" && -x "$dir/export_mesh" && -x "$dir/export_water_mesh" && -x "$dir/export_models" ]]; then
        bin_dir=$dir
    fi
}

try_bin_dir "$script_dir"
try_bin_dir "$current_dir"
try_bin_dir "$script_dir/build/Release"
try_bin_dir "$script_dir/build"
try_bin_dir "$current_dir/build/Release"
try_bin_dir "$current_dir/build"
try_bin_dir "$script_dir/../build/Release"
try_bin_dir "$script_dir/../build"
try_bin_dir "$script_dir/build-linux"
try_bin_dir "$current_dir/build-linux"
try_bin_dir "$script_dir/../build-linux"

if [[ -z $bin_dir ]]; then
    cat >&2 <<EOF
Could not find the extraction executables.
Checked:
  "$script_dir"
  "$current_dir"
  "$script_dir/build/Release"
  "$script_dir/build"
  "$current_dir/build/Release"
  "$current_dir/build"
  "$script_dir/../build/Release"
  "$script_dir/../build"
  "$script_dir/build-linux"
  "$current_dir/build-linux"
  "$script_dir/../build-linux"

Expected to find extract_map, export_mesh, export_water_mesh, and export_models in the same folder.
EOF
    exit 1
fi

echo "Extracting map assets..."
"$bin_dir/extract_map" "$tags_folder" "$mesh_tag" --out "$out_folder" "${extract_args[@]}"

echo "Exporting terrain mesh..."
"$bin_dir/export_mesh" "$out_folder"

echo "Exporting water mesh..."
"$bin_dir/export_water_mesh" "$out_folder"

echo "Exporting scenery models..."
"$bin_dir/export_models" "$tags_folder" "$out_folder" "$out_folder/assets/models/displacement.obj" "${model_args[@]}"

echo "Done. Output written to \"$out_folder\"."
