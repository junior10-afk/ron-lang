# Builds the Ron compiler (ronc), the engine DLL and runs the tests.
# Usage:  .\build.ps1             (Debug)
#         .\build.ps1 -Release
#         .\build.ps1 -NoTests

param([switch]$Release, [switch]$NoTests)
$ErrorActionPreference = 'Stop'

$config = if ($Release) { 'Release' } else { 'Debug' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ build tools not found." }
$cmakeDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"

Push-Location $PSScriptRoot
try {
    & "$cmakeDir\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64 | Out-Null
    if ($LASTEXITCODE) { throw "CMake configuration failed" }
    & "$cmakeDir\cmake.exe" --build build --config $config -- /nologo /verbosity:minimal
    if ($LASTEXITCODE) { throw "Build failed" }
    if (-not $NoTests) {
        & "$cmakeDir\ctest.exe" --test-dir build -C $config --output-on-failure
        if ($LASTEXITCODE) { throw "Tests failed" }
    }
    Write-Host "`nOK: build\$config\ronc.exe" -ForegroundColor Green
} finally {
    Pop-Location
}
