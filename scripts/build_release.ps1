param(
    [switch]$SkipFirmware,
    [switch]$SkipDll,
    [switch]$NoPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$mingw32Bin = "C:\msys64\mingw32\bin"
$gcc32 = Join-Path $mingw32Bin "i686-w64-mingw32-gcc.exe"
$artifactDir = Join-Path $repoRoot "artifacts"
$releaseZip = Join-Path $artifactDir "chunithm-release.zip"

function Assert-LastExitCode {
    param(
        [int]$ExitCode,
        [string]$StepName
    )

    if ($ExitCode -ne 0) {
        throw "$StepName failed with exit code $ExitCode"
    }
}

function Get-PeMachine {
    param(
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

Push-Location $repoRoot
try {
    if (-not $SkipFirmware) {
        $idf = Get-Command idf.py -ErrorAction SilentlyContinue
        if ($null -eq $idf) {
            throw "idf.py not found. Open ESP-IDF PowerShell first, then rerun this script."
        }

        Write-Host "[1/3] Building ESP-IDF firmware..."
        & idf.py build
        Assert-LastExitCode -ExitCode $LASTEXITCODE -StepName "ESP-IDF build"
    }

    if (-not $SkipDll) {
        if (-not (Test-Path $gcc32)) {
            throw "x86 gcc not found at $gcc32"
        }

        Write-Host "[2/3] Building chuniio x86 DLL..."
        $env:PATH = "$mingw32Bin;$env:PATH"
        & $gcc32 -shared -O2 -s -o "chuniio/chuniio.dll" "chuniio/chuniio.c"
        Assert-LastExitCode -ExitCode $LASTEXITCODE -StepName "chuniio x86 DLL build"

        $machine = Get-PeMachine -Path (Join-Path $repoRoot "chuniio/chuniio.dll")
        if ($machine -ne 0x014C) {
            throw ("Built DLL is not x86. PE machine: 0x{0:X4}" -f $machine)
        }
    }

    if (-not $NoPackage) {
        Write-Host "[3/3] Packaging artifacts for GitHub upload..."

        New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null

        $filesToCopy = @(
            "build/chunithm.bin",
            "build/bootloader/bootloader.bin",
            "build/partition_table/partition-table.bin",
            "chuniio/chuniio.dll"
        )

        foreach ($relativePath in $filesToCopy) {
            $fullPath = Join-Path $repoRoot $relativePath
            if (Test-Path $fullPath) {
                Copy-Item -Path $fullPath -Destination $artifactDir -Force
            } else {
                Write-Warning "Missing artifact: $relativePath"
            }
        }

        if (Test-Path $releaseZip) {
            Remove-Item -Path $releaseZip -Force
        }

        $zipInputs = @(
            (Join-Path $artifactDir "chunithm.bin"),
            (Join-Path $artifactDir "bootloader.bin"),
            (Join-Path $artifactDir "partition-table.bin"),
            (Join-Path $artifactDir "chuniio.dll")
        ) | Where-Object { Test-Path $_ }

        if ($zipInputs.Count -gt 0) {
            Compress-Archive -Path $zipInputs -DestinationPath $releaseZip -Force
            Write-Host "Created: $releaseZip"
        } else {
            Write-Warning "No artifacts found to zip."
        }
    }

    Write-Host "Build script completed successfully."
}
finally {
    Pop-Location
}
