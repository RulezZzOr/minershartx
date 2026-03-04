[CmdletBinding()]
param(
    [switch]$SkipVisualStudio,
    [switch]$SkipCuda
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-Winget {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget was not found. Install App Installer from Microsoft Store first."
    }
}

function Install-WingetPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Id,
        [string[]]$AdditionalArgs = @()
    )

    Write-Host "Installing $Id ..." -ForegroundColor Cyan
    $args = @(
        'install', '--id', $Id, '--exact', '--source', 'winget',
        '--accept-source-agreements', '--accept-package-agreements'
    ) + $AdditionalArgs

    & winget @args
}

function Try-InstallCuda {
    $candidates = @(
        'NVIDIA.CUDA',
        'Nvidia.CUDA',
        'NVIDIA.CUDAToolkit'
    )

    foreach ($candidate in $candidates) {
        try {
            Install-WingetPackage -Id $candidate
            return $true
        }
        catch {
            Write-Warning "CUDA install via '$candidate' failed, trying next candidate..."
        }
    }

    return $false
}

Require-Winget

# Core build tools
Install-WingetPackage -Id 'Kitware.CMake'
Install-WingetPackage -Id 'Ninja-build.Ninja'
Install-WingetPackage -Id 'Git.Git'

if (-not $SkipVisualStudio) {
    # MSVC toolchain + Windows SDK
    Install-WingetPackage -Id 'Microsoft.VisualStudio.2022.BuildTools' -AdditionalArgs @(
        '--override',
        '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621'
    )
}

if (-not $SkipCuda) {
    $cudaOk = Try-InstallCuda
    if (-not $cudaOk) {
        Write-Warning 'CUDA toolkit was not installed automatically via winget.'
        Write-Warning 'Install CUDA manually from https://developer.nvidia.com/cuda-downloads'
    }
}

Write-Host ''
Write-Host 'Versions detected in current shell:' -ForegroundColor Green

if (Get-Command cmake -ErrorAction SilentlyContinue) {
    cmake --version | Select-Object -First 1
} else {
    Write-Warning 'cmake not found in current PATH (open a new terminal after install).'
}

if (Get-Command ninja -ErrorAction SilentlyContinue) {
    ninja --version
} else {
    Write-Warning 'ninja not found in current PATH (open a new terminal after install).'
}

if (Get-Command nvcc -ErrorAction SilentlyContinue) {
    nvcc --version | Select-Object -First 4
} else {
    Write-Warning 'nvcc not found in current PATH (new shell or manual CUDA install may be needed).'
}

Write-Host ''
Write-Host 'Done. If PATH changed, close and reopen PowerShell.' -ForegroundColor Green
