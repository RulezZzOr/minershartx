#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exe_path="${repo_root}/build/minershartx"

if [[ ! -x "$exe_path" ]]; then
  echo "ERROR: $exe_path not found or not executable." >&2
  echo "Build first with scripts/build_linux.sh" >&2
  exit 1
fi

rpc_url="${MINER_RPC_URL:-http://127.0.0.1:8332}"
address="${MINER_ADDRESS:-}"
rpc_user="${MINER_RPC_USER:-}"
rpc_pass="${MINER_RPC_PASS:-}"
rpc_cookie="${MINER_RPC_COOKIE:-}"
device="${MINER_DEVICE:-0}"
threads="${MINER_THREADS:-256}"
blocks="${MINER_BLOCKS:-4080}"
chunk_nonces="${MINER_CHUNK_NONCES:-4294967296}"

cmd=(
  "$exe_path"
  --mode solo
  --rpc-url "$rpc_url"
  --device "$device"
  --threads "$threads"
  --blocks "$blocks"
  --chunk-nonces "$chunk_nonces"
)

if [[ -n "$address" ]]; then
  cmd+=(--address "$address")
fi
if [[ -n "$rpc_user" ]]; then
  cmd+=(--rpc-user "$rpc_user")
fi
if [[ -n "$rpc_pass" ]]; then
  cmd+=(--rpc-pass "$rpc_pass")
fi
if [[ -n "$rpc_cookie" ]]; then
  cmd+=(--rpc-cookie "$rpc_cookie")
fi

echo "Starting miner in solo mode..."
echo "RPC: $rpc_url"
echo "Device: $device Threads: $threads Blocks: $blocks Chunk: $chunk_nonces"
if [[ -n "$address" ]]; then
  echo "Payout address: $address"
fi
if [[ -n "$rpc_cookie" ]]; then
  echo "RPC cookie: $rpc_cookie"
fi
if [[ -n "$rpc_user" ]]; then
  echo "RPC user: $rpc_user"
fi
echo

"${cmd[@]}"
