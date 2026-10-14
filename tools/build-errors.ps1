# Runs build.ps1 and prints only errors (handy for iterating).
$log = Join-Path $env:TEMP "cc_build.txt"
try { & (Join-Path $PSScriptRoot "..\build.ps1") *> $log } catch {}
$lines = Select-String -Path $log -Pattern "error|FAILED|fatal|CMake Error|Built " | ForEach-Object { $_.Line }
$lines | Select-Object -First 60
