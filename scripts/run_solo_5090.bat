@echo off
setlocal

set "REPO_DIR=%~dp0.."
set "EXE=%REPO_DIR%\build\minershartx.exe"

if not exist "%EXE%" (
  echo ERROR: "%EXE%" not found.
  echo Build first with scripts\build_windows.ps1
  pause
  exit /b 1
)

if not defined MINER_RPC_URL set "MINER_RPC_URL=http://127.0.0.1:8332"
if not defined MINER_DEVICE set "MINER_DEVICE=0"
if not defined MINER_THREADS set "MINER_THREADS=256"
if not defined MINER_BLOCKS set "MINER_BLOCKS=4080"
if not defined MINER_CHUNK_NONCES set "MINER_CHUNK_NONCES=4294967296"

set "RPC_URL=%MINER_RPC_URL%"
set "ADDRESS=%MINER_ADDRESS%"
set "RPC_USER=%MINER_RPC_USER%"
set "RPC_PASS=%MINER_RPC_PASS%"
set "RPC_COOKIE=%MINER_RPC_COOKIE%"
set "DEVICE=%MINER_DEVICE%"
set "THREADS=%MINER_THREADS%"
set "BLOCKS=%MINER_BLOCKS%"
set "CHUNK_NONCES=%MINER_CHUNK_NONCES%"

echo Starting miner in solo mode on RTX 5090...
echo RPC: %RPC_URL%
if defined ADDRESS echo Payout address: %ADDRESS%
if defined RPC_COOKIE echo RPC cookie: %RPC_COOKIE%
if defined RPC_USER echo RPC user: %RPC_USER%
echo.

"%EXE%" --mode solo --rpc-url "%RPC_URL%" --device %DEVICE% --threads %THREADS% --blocks %BLOCKS% --chunk-nonces %CHUNK_NONCES% --address "%ADDRESS%" --rpc-user "%RPC_USER%" --rpc-pass "%RPC_PASS%" --rpc-cookie "%RPC_COOKIE%"

set "EXITCODE=%ERRORLEVEL%"
echo.
echo Miner exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
