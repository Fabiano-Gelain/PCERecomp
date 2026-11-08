# Builds PCERecomp standalone (Release) into build\ and, with -Test, runs its tests.
# (Inside a game project, the game's build.ps1 does the build.)
# Usage:  .\build.ps1          -> build
#         .\build.ps1 -Test    -> build and run the tests
#         -Verbose             -> show all output
param([switch]$Test, [switch]$Verbose)
$ErrorActionPreference = 'Stop'

$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
if (-not (Test-Path $cmake)) { throw "CMake not found at $cmake" }

function Run-Filtered([string]$exe, [string[]]$arguments, [string]$filter) {
    if ($Verbose) {
        & $exe @arguments | ForEach-Object { Write-Host $_ }
    } else {
        $out = & $exe @arguments 2>&1 | ForEach-Object { "$_" }
        $out | Select-String -Pattern $filter | ForEach-Object { Write-Host $_.Line }
    }
    return $LASTEXITCODE
}

Set-Location $PSScriptRoot
if (-not (Test-Path 'build\CMakeCache.txt')) {
    $rc = Run-Filtered $cmake @('-S', '.', '-B', 'build', '-G', 'Visual Studio 17 2022', '-A', 'x64') '(?i)error|warning'
    if ($rc -ne 0) { throw 'CMake configure failed' }
}
$rc = Run-Filtered $cmake @('--build', 'build', '--config', 'Release', '--parallel') '(?i)warning C\d|error|: fatal'
if ($rc -ne 0) { throw 'Build failed' }
Write-Host 'Build OK.'

if ($Test) {
    $targs = @('--test-dir', 'build', '-C', 'Release', '-j', [Math]::Max(1, [Environment]::ProcessorCount / 2))
    if ($Verbose) { $targs += '--output-on-failure' }
    $rc = Run-Filtered $ctest $targs '(?i)tests passed|\*\*\*Failed|\*\*\*Exception|\*\*\*Timeout|Not Run|^\s+\d+ - '
    if ($rc -ne 0) { throw 'Tests failed' }
}
