[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$BuildType = 'Release',
    [string]$CudaArch = 'native'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-ExecutablePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandName,
        [string[]]$Candidates = @()
    )

    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) {
        return $cmd.Source
    }

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

$cmakePath = Resolve-ExecutablePath -CommandName 'cmake' -Candidates @(
    (Join-Path ${env:ProgramFiles} 'CMake\bin\cmake.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\cmake.exe')
)
if (-not $cmakePath) {
    throw "cmake.exe was not found. Install tools with .\scripts\setup_windows_tools.ps1 first."
}

$ninjaPath = Resolve-ExecutablePath -CommandName 'ninja' -Candidates @(
    (Join-Path ${env:ProgramFiles} 'Ninja\ninja.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Ninja\ninja.exe'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ninja.exe')
)
if (-not $ninjaPath) {
    throw "ninja.exe was not found. Install tools with .\scripts\setup_windows_tools.ps1 first."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found at '$vswhere'. Install Visual Studio Build Tools first."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'No Visual Studio installation with C++ tools was found.'
}

$vsDevCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $vsDevCmd)) {
    throw "VsDevCmd.bat not found at '$vsDevCmd'."
}

$cmdParts = @(
    "call `"$vsDevCmd`" -arch=x64",
    "`"$cmakePath`" -S `"$repoRoot`" -B `"$buildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninjaPath`" -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_CUDA_ARCHITECTURES=$CudaArch",
    "`"$cmakePath`" --build `"$buildDir`" --config $BuildType"
)

$cmdLine = $cmdParts -join ' && '
cmd.exe /c $cmdLine
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$exePath = Join-Path $buildDir 'minershartx.exe'
if (Test-Path $exePath) {
    Write-Host "Build OK: $exePath" -ForegroundColor Green
} else {
    throw "Build finished but executable was not found at $exePath"
}
