#!/usr/bin/env bash
set -euo pipefail

build_type="Release"
cuda_arch="native"
clean_build=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-type)
      build_type="${2:-}"
      shift 2
      ;;
    --cuda-arch)
      cuda_arch="${2:-}"
      shift 2
      ;;
    --clean)
      clean_build=1
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--build-type Release|RelWithDebInfo|Debug] [--cuda-arch native|<sm>] [--clean]" >&2
      exit 1
      ;;
  esac
done

case "$build_type" in
  Release|RelWithDebInfo|Debug) ;;
  *)
    echo "Invalid --build-type: $build_type" >&2
    exit 1
    ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found in PATH" >&2
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  echo "ninja not found in PATH" >&2
  echo "Install Ninja and retry." >&2
  exit 1
fi

cuda_root="${CUDAToolkit_ROOT:-${CUDA_HOME:-${CUDA_PATH:-}}}"
nvcc_path=""

if [[ -n "$cuda_root" && -x "$cuda_root/bin/nvcc" ]]; then
  nvcc_path="${cuda_root}/bin/nvcc"
elif command -v nvcc >/dev/null 2>&1; then
  nvcc_path="$(command -v nvcc)"
  cuda_root="$(cd "$(dirname "$nvcc_path")/.." && pwd)"
elif [[ -n "$cuda_root" && -x "$cuda_root/bin/nvcc" ]]; then
  nvcc_path="${cuda_root}/bin/nvcc"
fi

if [[ -z "$nvcc_path" || -z "$cuda_root" ]]; then
  echo "nvcc not found. Set CUDA_HOME, CUDA_PATH, or CUDAToolkit_ROOT, or add nvcc to PATH." >&2
  exit 1
fi

if [[ "$clean_build" -eq 1 ]]; then
  rm -rf "$build_dir"
fi

jobs="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

cmake -S "$repo_root" \
      -B "$build_dir" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE="$build_type" \
      -DCMAKE_CUDA_ARCHITECTURES="$cuda_arch" \
      -DCUDAToolkit_ROOT="$cuda_root" \
      -DCMAKE_CUDA_COMPILER="$nvcc_path"

cmake --build "$build_dir" --config "$build_type" -j"$jobs"

exe_path="${build_dir}/minershartx"
if [[ -x "$exe_path" ]]; then
  echo "Build OK: $exe_path"
else
  echo "Build finished but executable not found at $exe_path" >&2
  exit 1
fi
