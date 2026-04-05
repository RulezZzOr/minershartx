# minershartx (CUDA SHA-256d miner prototype for Windows + Linux)

This project now contains:

- `benchmark` mode: raw CUDA `SHA256d` throughput test (`GH/s`)
- `pool` mode: Stratum v1 client + CUDA nonce scanning + `mining.submit`
- `solo` mode: Bitcoin Core JSON-RPC `getblocktemplate` + CUDA nonce scanning + `submitblock`
- persistent CUDA scan engine (device buffers reused across chunks)

It supports **NVIDIA CUDA on Windows and Linux**.

## Important notes

- This is an experimental miner core, not production-grade yet.
- For BTC `SHA-256`, GPUs are typically far behind modern ASIC miners.
- Current share target calculation uses integer difficulty (`set_difficulty` is floored).
- Pool mode can also request a minimum difficulty floor with `--pool-difficulty <value>`.
  The pool may ignore the suggestion, but the miner will not scan below that floor.

## Requirements (Windows)

- Windows 10/11
- NVIDIA driver with CUDA support
- CUDA Toolkit 12.x
- CMake 3.24+
- Visual Studio 2022 Build Tools (MSVC) or Ninja + MSVC

## Requirements (Linux)

- Linux (tested on Ubuntu-like environments)
- NVIDIA driver with CUDA support
- CUDA Toolkit with `nvcc` in `PATH`
- `cmake` 3.24+
- `ninja-build`
- C++ toolchain (`g++`/`clang++`)

## Automatic Tool Install (Windows)

Run this once in **PowerShell (Administrator)**:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\scripts\setup_windows_tools.ps1
```

This installs:

- CMake
- Ninja
- Git
- Visual Studio 2022 Build Tools (C++ workload + Windows SDK)
- CUDA Toolkit (best effort via `winget`, fallback message if manual install is needed)

## Build (PowerShell)

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=native
cmake --build build --config Release
```

Or use the helper script:

```powershell
.\scripts\build_windows.ps1 -BuildType Release -CudaArch native
```

If your terminal blocks script execution or you moved the repo directory, use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_windows.ps1 -BuildType Release -CudaArch native -Clean
```

If `native` does not work in your toolchain, set explicit architecture, for example:

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=90
```

## Build (Linux)

Manual:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=native
cmake --build build -j"$(nproc)"
```

Helper script:

```bash
chmod +x scripts/build_linux.sh
./scripts/build_linux.sh --build-type Release --cuda-arch native
```

The Linux helper auto-detects `nvcc` from `PATH`, `CUDA_HOME`, `CUDA_PATH`, or
`CUDAToolkit_ROOT` and passes the detected toolkit root to CMake.

## JSON Config

You can also load settings from a JSON file and then override them with CLI
arguments.

Example solo config:

```json
{
  "mode": "solo",
  "device": "all",
  "threads": 256,
  "blocks": 4080,
  "chunk_nonces": 4294967296,
  "rpc_url": "http://127.0.0.1:8332",
  "rpc_user": "tata",
  "rpc_pass": "secret",
  "address": "bc1..."
}
```

You can copy `examples/solo.example.json` as a starting point.

Example pool config:

```json
{
  "mode": "pool",
  "device": "all",
  "threads": 256,
  "blocks": 4080,
  "chunk_nonces": 4294967296,
  "pool": "poolflix.eu:3333",
  "user": "bc1...worker",
  "pass": "x",
  "pool_difficulty": 10000,
  "debug_pool_header": true
}
```

You can copy `examples/pool.example.json` as a starting point.

Load it with:

```bash
./build/minershartx --config miner.json
```

or:

```powershell
.\build\minershartx.exe --config miner.json
```

Helper scripts also honor `MINER_CONFIG`:

```bash
MINER_CONFIG=miner.json MINER_USER="bc1...worker" ./scripts/run_pool_linux.sh
```

```powershell
$env:MINER_CONFIG="miner.json"
.\scripts\run_solo_5090.bat
```

## Self-test

The repo now also builds a small CPU-only correctness check, similar in spirit to the
test/benchmark split in `tetsuo-gpu-miner`.

```powershell
cmake --build build --target minershartx_selftest
ctest --test-dir build --output-on-failure
```

```bash
cmake --build build --target minershartx_selftest
ctest --test-dir build --output-on-failure
```

## Benchmark mode

```powershell
.\build\minershartx.exe --mode benchmark --device 0 --seconds 15
```

```bash
./build/minershartx --mode benchmark --device 0 --seconds 15
```

Optional tuning:

```powershell
.\build\minershartx.exe --mode benchmark --device 0 --threads 256 --blocks 4096 --chunk-nonces 536870912 --seconds 20
```

Known good starting point for RTX 5090 (pool mode):

```powershell
.\build\minershartx.exe --mode pool --pool poolflix.eu:3333 --user <BTC_ADDRESS.WORKER> --pass x --pool-difficulty 10000 --device 0 --threads 256 --blocks 4080 --chunk-nonces 4294967296
```

## Pool mode (PoolFlix example)

```powershell
.\build\minershartx.exe --mode pool --pool poolflix.eu:5555 --user <BTC_ADDRESS.WORKER> --pass x --pool-difficulty 10000 --device 0
```

```bash
./build/minershartx --mode pool --pool poolflix.eu:5555 --user <BTC_ADDRESS.WORKER> --pass x --device 0
```

If shares are consistently rejected, try alternate nonce submit format:

```powershell
.\build\minershartx.exe --mode pool --pool poolflix.eu:5555 --user <BTC_ADDRESS.WORKER> --pass x --device 0 --nonce-submit-be
```

Pool dashboard prints a periodic stats table (`5s GH/s`, `Avg GH/s`, shares/rejects, diff, job)
and also `TempC/PowerW/Fan%` when `nvidia-smi` telemetry is available (otherwise `-`).

## Solo mode (Bitcoin Core node)

Solo mode mines against your own Bitcoin Core node and submits full blocks directly.
The block reward goes to the payout address you provide, or to a wallet address if you omit `--address`.
When the node advertises `longpollid`, the miner uses `getblocktemplate` longpoll to refresh work immediately.

```powershell
.\build\minershartx.exe --mode solo --rpc-url http://127.0.0.1:8332 --address <YOUR_BTC_ADDRESS> --device 0 --threads 256 --blocks 4080 --chunk-nonces 4294967296
```

```bash
./build/minershartx --mode solo --rpc-url http://127.0.0.1:8332 --address <YOUR_BTC_ADDRESS> --device 0
```

RPC authentication:

- If your Bitcoin Core node uses the default cookie auth, the miner auto-detects `.cookie`
- You can also pass `--rpc-user` and `--rpc-pass`
- Use `--rpc-cookie <path>` if your cookie file is in a custom datadir
- `--rpc-url` may include a path, for example `http://127.0.0.1:8332/wallet/<walletname>`
- If you provide `--address`, standard Bitcoin addresses are decoded locally and do not require wallet RPC access for payout-script resolution
- If you omit `--address`, the miner still needs wallet RPC access to call `getnewaddress`

If you omit `--address`, the miner calls `getnewaddress` on the wallet RPC and pays the block reward there.

Solo helper scripts:

```powershell
.\scripts\run_solo_5090.bat
```

```bash
chmod +x scripts/run_solo_linux.sh
./scripts/run_solo_linux.sh
```

Optional Linux env vars for `run_solo_linux.sh`:

- `MINER_CONFIG` (optional JSON config file)
- `MINER_RPC_URL` (default: `http://127.0.0.1:8332`)
- `MINER_ADDRESS` (default: wallet `getnewaddress`)
- `MINER_RPC_USER`
- `MINER_RPC_PASS`
- `MINER_RPC_COOKIE`
- `MINER_DEVICE` (default: `all`)
- `MINER_THREADS` (default: `256`)
- `MINER_BLOCKS` (default: `4080`)
- `MINER_CHUNK_NONCES` (default: `4294967296`)

Linux pool run helper:

```bash
chmod +x scripts/run_pool_linux.sh
MINER_USER="<BTC_ADDRESS.WORKER>" ./scripts/run_pool_linux.sh
```

For header debugging:

```bash
MINER_USER="<BTC_ADDRESS.WORKER>" MINER_DEBUG_POOL_HEADER=1 ./scripts/run_pool_linux.sh
```

The Windows pool helper scripts also honor `MINER_DEBUG_POOL_HEADER=1`.

By default, the Linux helpers mine on all detected CUDA devices. Override with
`MINER_DEVICE=0` or `MINER_DEVICE=0,1` if you want a subset.

Optional Linux env vars for `run_pool_linux.sh`:

- `MINER_CONFIG` (optional JSON config file)
- `MINER_POOL` (default: `poolflix.eu:3333`)
- `MINER_USER` (required)
- `MINER_PASS` (default: `x`)
- `MINER_DEVICE` (default: `all`)
- `MINER_THREADS` (default: `256`)
- `MINER_BLOCKS` (default: `4080`)
- `MINER_CHUNK_NONCES` (default: `4294967296`)
- `MINER_NONCE_BE` (`1` or `0`, default: `0`)
- `MINER_POOL_DIFFICULTY` (default: `10000`)
- `MINER_DEBUG_POOL_HEADER` (`1` or `0`, default: `0`)

## CLI options

- `--mode <benchmark|pool|solo>`
- `--config <path>` (load JSON config first)
- `--device <id>`
- `--threads <value>` (32..1024, multiple of 32)
- `--blocks <value>` (0 = auto)
- `--chunk-nonces <value>` (0 = auto)
- `--seconds <value>` (benchmark mode)
- `--pool <host:port>` (pool mode)
- `--pool-difficulty <value>` (pool mode, request/floor difficulty)
- `--debug-pool-header` (pool mode, print 80-byte header and components)
- `--user <username>` (pool mode, required)
- `--pass <password>` (pool mode)
- `--nonce-submit-be` (pool mode)
- `--rpc-url <url>` (solo mode)
- `--rpc-user <username>` (solo mode, optional with cookie auth)
- `--rpc-pass <password>` (solo mode, optional with cookie auth)
- `--rpc-cookie <path>` (solo mode)
- `--address <address>` (solo mode, optional if wallet RPC can generate one)
- `--help`

## Known limitations

- Experimental codebase
- No `mining.set_extranonce` handling yet
