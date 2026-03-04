# minershartx (Windows-first CUDA SHA-256d miner prototype)

This project now contains:

- `benchmark` mode: raw CUDA `SHA256d` throughput test (`GH/s`)
- `pool` mode: Stratum v1 client + CUDA nonce scanning + `mining.submit`
- persistent CUDA scan engine (device buffers reused across chunks)

It is focused on **Windows + NVIDIA CUDA** first.

## Important notes

- This is an experimental miner core, not production-grade yet.
- For BTC `SHA-256`, GPUs are typically far behind modern ASIC miners.
- Current share target calculation uses integer difficulty (`set_difficulty` is floored).

## Requirements (Windows)

- Windows 10/11
- NVIDIA driver with CUDA support
- CUDA Toolkit 12.x
- CMake 3.24+
- Visual Studio 2022 Build Tools (MSVC) or Ninja + MSVC

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

If `native` does not work in your toolchain, set explicit architecture, for example:

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=90
```

## Benchmark mode

```powershell
.\build\minershartx.exe --mode benchmark --device 0 --seconds 15
```

Optional tuning:

```powershell
.\build\minershartx.exe --mode benchmark --device 0 --threads 256 --blocks 4096 --chunk-nonces 536870912 --seconds 20
```

## Pool mode (PoolFlix example)

```powershell
.\build\minershartx.exe --mode pool --pool poolflix.eu:5555 --user <BTC_ADDRESS.WORKER> --pass x --device 0
```

If shares are consistently rejected, try alternate nonce submit format:

```powershell
.\build\minershartx.exe --mode pool --pool poolflix.eu:5555 --user <BTC_ADDRESS.WORKER> --pass x --device 0 --nonce-submit-be
```

## CLI options

- `--mode <benchmark|pool>`
- `--device <id>`
- `--threads <value>` (32..1024, multiple of 32)
- `--blocks <value>` (0 = auto)
- `--chunk-nonces <value>` (0 = auto)
- `--seconds <value>` (benchmark mode)
- `--pool <host:port>` (pool mode)
- `--user <username>` (pool mode, required)
- `--pass <password>` (pool mode)
- `--nonce-submit-be` (pool mode)
- `--help`

## Known limitations

- Single-GPU process
- No reconnect/backoff loop yet
- No `mining.set_extranonce` handling yet
