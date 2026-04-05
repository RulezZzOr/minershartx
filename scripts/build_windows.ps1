[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$BuildType = 'Release',
    [string]$CudaArch = 'native',
    [switch]$Clean
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

function Normalize-PathString {
    param(
        [string]$PathValue
    )

    if (-not $PathValue) {
        return ''
    }

    try {
        return ([System.IO.Path]::GetFullPath($PathValue)).TrimEnd('\').ToLowerInvariant()
    } catch {
        return $PathValue.TrimEnd('\').ToLowerInvariant()
    }
}

function Get-CMakeHomeDirectory {
    param(
        [string]$BuildDirectory
    )

    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path $cachePath)) {
        return $null
    }

    foreach ($line in Get-Content -Path $cachePath -ErrorAction SilentlyContinue) {
        if ($line -match '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$') {
            return $matches[1]
        }
    }

    return $null
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

$repoRootNorm = Normalize-PathString $repoRoot
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning build directory: $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

$cacheHome = Get-CMakeHomeDirectory -BuildDirectory $buildDir
if ($cacheHome) {
    $cacheHomeNorm = Normalize-PathString $cacheHome
    if ($cacheHomeNorm -ne $repoRootNorm) {
        Write-Warning "Detected stale CMake cache from different source directory: $cacheHome"
        Write-Host "Removing stale build directory: $buildDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }
}

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

$cudaCandidates = @()
if ($env:CUDA_PATH) {
    $cudaCandidates += (Join-Path $env:CUDA_PATH 'bin\nvcc.exe')
}
if ($env:CUDAToolkit_ROOT) {
    $cudaCandidates += (Join-Path $env:CUDAToolkit_ROOT 'bin\nvcc.exe')
}

$cudaToolkitBase = Join-Path ${env:ProgramFiles} 'NVIDIA GPU Computing Toolkit\CUDA'
if (Test-Path $cudaToolkitBase) {
    $cudaCandidates += Get-ChildItem -Path $cudaToolkitBase -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\nvcc.exe' }
}

$nvccPath = Resolve-ExecutablePath -CommandName 'nvcc' -Candidates $cudaCandidates
$cudaRoot = $null
if ($nvccPath) {
    $cudaRoot = Split-Path (Split-Path $nvccPath -Parent) -Parent
    Write-Host "Using nvcc: $nvccPath"
} else {
    Write-Warning "nvcc not found in PATH/common locations; CMake configure may fail if CUDA toolkit is missing."
}

$cmakeConfigureArgs = @(
    "-S `"$repoRoot`"",
    "-B `"$buildDir`"",
    "-G Ninja",
    "-DCMAKE_MAKE_PROGRAM=`"$ninjaPath`"",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_CUDA_ARCHITECTURES=$CudaArch"
)

if ($cudaRoot -and $nvccPath) {
    $cmakeConfigureArgs += "-DCUDAToolkit_ROOT=`"$cudaRoot`""
    $cmakeConfigureArgs += "-DCMAKE_CUDA_COMPILER=`"$nvccPath`""
}

$cmdParts = @(
    "call `"$vsDevCmd`" -arch=x64",
    "`"$cmakePath`" $($cmakeConfigureArgs -join ' ')",
    "`"$cmakePath`" --build `"$buildDir`" --config $BuildType"
)

Write-Host "Using cmake: $cmakePath"
Write-Host "Using ninja: $ninjaPath"

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
