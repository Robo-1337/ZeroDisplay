# Builds out\ZeroDisplay.exe using the installed Visual Studio C++ Build Tools.
param(
    [ValidateSet("Release", "Debug")] [string]$Config = "Release",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$build = Join-Path $root "build\$Config"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

# vcvars calls vswhere itself; make sure it can find it.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$ErrorActionPreference = "Continue"
cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`"" 2>&1 | ForEach-Object { "$_" }
$code = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($code -ne 0) { throw "Build failed" }

$out = Join-Path $root "out"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $build "ZeroDisplay.exe") (Join-Path $out "ZeroDisplay.exe") -Force
Write-Host "Built $out\ZeroDisplay.exe"
