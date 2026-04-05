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

set "POOL=poolflix.eu:3333"
set "USER=bc1qn2q5pede0fk3ul4v4ajw5eaavzjn6uy66f6fdr.rtx5090"
set "PASS=x"
set "POOL_DIFFICULTY=10000"
set "DEBUG_POOL_HEADER=%MINER_DEBUG_POOL_HEADER%"
if not defined DEBUG_POOL_HEADER set "DEBUG_POOL_HEADER=0"
set "DEBUG_ARG="
if "%DEBUG_POOL_HEADER%"=="1" set "DEBUG_ARG=--debug-pool-header"

echo Starting miner on RTX 5090...
echo Pool: %POOL%
echo User: %USER%
echo Pool difficulty: %POOL_DIFFICULTY%
echo.

"%EXE%" --mode pool --pool %POOL% --user %USER% --pass %PASS% --pool-difficulty %POOL_DIFFICULTY% %DEBUG_ARG% --device 0 --threads 256 --blocks 4080 --chunk-nonces 4294967296

set "EXITCODE=%ERRORLEVEL%"
echo.
echo Miner exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
