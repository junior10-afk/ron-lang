# Builds Ron in Release mode and packages a ready-to-use zip:
#   ron-<version>-windows-x64/
#     ronc.exe  ron_engine.dll  std/  examples/  assets/  docs/  editors/  README.md  LICENSE  CHANGELOG.md
# Usage: .\package.ps1

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & .\build.ps1 -Release
    if ($LASTEXITCODE) { throw "Build or tests failed" }

    $version = (& .\build\Release\ronc.exe --version).Split(' ')[1]
    $name = "ron-$version-windows-x64"
    $stage = Join-Path "build" $name
    if (Test-Path $stage) { Get-ChildItem $stage -Recurse | Remove-Item -Recurse -Force }
    New-Item -ItemType Directory -Force $stage | Out-Null

    Copy-Item build\Release\ronc.exe, build\Release\ron_engine.dll $stage
    Copy-Item -Recurse std, examples, assets, docs, editors (Join-Path $stage '.')
    Copy-Item README.md, LICENSE, CHANGELOG.md $stage
    # Downloaded test models (assets/samples, not in git) are not ours to ship
    $samples = Join-Path $stage 'assets\samples'
    if (Test-Path $samples) { Remove-Item $samples -Recurse -Force }
    # Drop build products that may sit next to the examples
    Get-ChildItem $stage -Recurse -Include *.exe | Where-Object { $_.Name -ne 'ronc.exe' } | Remove-Item -Force
    Get-ChildItem (Join-Path $stage 'examples') -Recurse -Include *.dll | Remove-Item -Force

    $zip = Join-Path "build" "$name.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $stage -DestinationPath $zip
    Write-Host "`nPackaged $zip" -ForegroundColor Green
} finally {
    Pop-Location
}
