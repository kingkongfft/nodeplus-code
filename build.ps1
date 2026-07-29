# build.ps1 - MinGW-w64 incremental build script for nodeplus-code
#
# Usage:
#   .\build.ps1               # incremental release build (default)
#   .\build.ps1 -Clean        # full clean release build
#   .\build.ps1 -Debug        # incremental debug build
#   .\build.ps1 -Clean -Debug # full clean debug build
#   .\build.ps1 -Clang        # use clang++ instead of g++
#   .\build.ps1 -Verbose      # show every compiler command

param(
    [switch]$Clean,
    [switch]$Debug,
    [switch]$Clang,
    [switch]$Verbose
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Paths
$RepoRoot    = $PSScriptRoot
$GccDir      = Join-Path $RepoRoot 'PowerEditor\gcc'
$BuildTemp   = Join-Path $RepoRoot '.build_temp'
$Makefile    = Join-Path $GccDir 'makefile'
$PreBuildBat = Join-Path $RepoRoot 'PowerEditor\src\NppLibsVersionH-generator.bat'
$MingwBin    = 'C:\msys64\mingw64\bin'
$MsysBin     = 'C:\msys64\usr\bin'

# Validation
if (-not (Test-Path $Makefile)) {
    Write-Error ("makefile not found at '{0}'. Run from the repository root." -f $Makefile)
}

$Make = Join-Path $MingwBin 'mingw32-make.exe'
if (-not (Test-Path $Make)) {
    Write-Error ("mingw32-make.exe not found in '{0}'. Install MSYS2 + mingw-w64-x86_64-toolchain." -f $MingwBin)
}

# Prepend MinGW + MSYS2 to PATH
$env:PATH = $MingwBin + ';' + $MsysBin + ';' + $env:PATH

# Ensure build temp dir exists
New-Item -ItemType Directory -Force -Path $BuildTemp | Out-Null

# Clean if requested
if ($Clean) {
    Write-Host 'Removing previous build output...' -ForegroundColor Yellow
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $BuildTemp '*')
    Get-ChildItem -Path $GccDir -Directory |
        Where-Object { $_.Name -match '^bin\.(gcc|clang)\.' } |
        ForEach-Object { Remove-Item -Recurse -Force $_.FullName }
}

# Pre-build: generate NppLibsVersion.h
Write-Host 'Generating library version header...' -ForegroundColor Cyan
& cmd.exe /C $PreBuildBat
if ($LASTEXITCODE -ne 0) {
    Write-Error ('NppLibsVersionH-generator.bat failed (exit {0}).' -f $LASTEXITCODE)
}

# Build arguments
$Jobs = (Get-CimInstance -ClassName Win32_ComputerSystem).NumberOfLogicalProcessors
$MakeArgs = @(('-j' + $Jobs), 'PREBUILD_EVENT_CMD=:')
if ($Debug)   { $MakeArgs += 'DEBUG=1' }
if ($Clang)   { $MakeArgs += 'CXX=clang++' }
if ($Verbose) { $MakeArgs += 'VERBOSE=1' }

# Print build info
$BuildLog = Join-Path $BuildTemp 'build.log'
$Mode     = if ($Clean)   { 'Full clean' } else { 'Incremental' }
$Variant  = if ($Debug)   { 'debug' }      else { 'release' }
$Compiler = if ($Clang)   { 'clang++' }    else { 'g++' }
Write-Host ('{0} build | {1} | {2} | {3} jobs' -f $Mode, $Variant, $Compiler, $Jobs) -ForegroundColor Green
Write-Host ('Build log: {0}' -f $BuildLog)

$sw = [Diagnostics.Stopwatch]::StartNew()

# Run make
# mingw32-make emits harmless jobserver warnings on stderr; redirect stderr->stdout
# so they appear in the log without triggering PowerShell's NativeCommandError.
Push-Location $GccDir
try {
    $ErrorActionPreference = 'Continue'
    & $Make @MakeArgs 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { $_ }
    } | Tee-Object -FilePath $BuildLog
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) {
        Write-Error ('Build FAILED (exit {0}). See log: {1}' -f $LASTEXITCODE, $BuildLog)
    }
} finally {
    $ErrorActionPreference = 'Stop'
    Pop-Location
}

$sw.Stop()
Write-Host ('Build completed successfully in {0}.' -f $sw.Elapsed.ToString('mm\:ss')) -ForegroundColor Green

# Report binary location
$BinSuffix   = if ($Clang) { 'clang' } else { 'gcc' }
$DebugSuffix = if ($Debug) { '-debug' } else { '' }
$BinDir = Join-Path $GccDir ('bin.{0}.x86_64{1}' -f $BinSuffix, $DebugSuffix)
$Binary = Join-Path $BinDir 'nodeplus-code.exe'
if (Test-Path $Binary) {
    $sizeMB = [math]::Round((Get-Item $Binary).Length / 1MB, 2)
    Write-Host ('Binary: {0} ({1} MB)' -f $Binary, $sizeMB) -ForegroundColor Cyan
}
