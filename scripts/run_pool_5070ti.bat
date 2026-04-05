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
set "USER=bc1qn2q5pede0fk3ul4v4ajw5eaavzjn6uy66f6fdr.rtx5070ti"
set "PASS=x"

echo Starting miner on RTX 5070 Ti...
echo Pool: %POOL%
echo User: %USER%
echo.

"%EXE%" --mode pool --pool %POOL% --user %USER% --pass %PASS% --device 0 --threads 256 --blocks 2560 --chunk-nonces 2147483648

set "EXITCODE=%ERRORLEVEL%"
echo.
echo Miner exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
