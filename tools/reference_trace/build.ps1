# Builds the reference trace tools (Geargrafx) into tools\reference_trace\build\.
$ErrorActionPreference = 'Stop'
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$dir = $PSScriptRoot
if (-not (Test-Path "$dir\build\CMakeCache.txt")) {
    & $cmake -S $dir -B "$dir\build" -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
}
& $cmake --build "$dir\build" --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
