#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exe_path="${repo_root}/build/minershartx"

if [[ ! -x "$exe_path" ]]; then
  echo "ERROR: $exe_path not found or not executable." >&2
  echo "Build first with scripts/build_linux.sh" >&2
  exit 1
fi

pool="${MINER_POOL:-poolflix.eu:3333}"
user="${MINER_USER:-}"
pass="${MINER_PASS:-x}"
device="${MINER_DEVICE:-all}"
threads="${MINER_THREADS:-256}"
blocks="${MINER_BLOCKS:-4080}"
chunk_nonces="${MINER_CHUNK_NONCES:-4294967296}"
nonce_be="${MINER_NONCE_BE:-0}"
pool_difficulty="${MINER_POOL_DIFFICULTY:-10000}"
debug_pool_header="${MINER_DEBUG_POOL_HEADER:-0}"

if [[ -z "$user" ]]; then
  echo "ERROR: MINER_USER is required (e.g. BTC_ADDRESS.worker)." >&2
  echo "Example:" >&2
  echo "  MINER_USER='bc1...myworker' scripts/run_pool_linux.sh" >&2
  exit 1
fi

cmd=(
  "$exe_path"
  --mode pool
  --pool "$pool"
  --user "$user"
  --pass "$pass"
  --pool-difficulty "$pool_difficulty"
  --device "$device"
  --threads "$threads"
  --blocks "$blocks"
  --chunk-nonces "$chunk_nonces"
)

if [[ "$nonce_be" == "1" ]]; then
  cmd+=(--nonce-submit-be)
fi
if [[ "$debug_pool_header" == "1" ]]; then
  cmd+=(--debug-pool-header)
fi

echo "Starting miner on Linux..."
echo "Pool: $pool"
echo "User: $user"
echo "Device: $device Threads: $threads Blocks: $blocks Chunk: $chunk_nonces Diff: $pool_difficulty"
echo "Debug pool header: $debug_pool_header"
echo

"${cmd[@]}"
