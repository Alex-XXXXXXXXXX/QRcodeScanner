$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $projectRoot 'third_party'
$zxingPath = Join-Path $thirdParty 'zxing-cpp'

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $zxingPath 'CMakeLists.txt'))) {
    git clone --branch v3.0.2 --depth 1 --recursive `
        https://github.com/zxing-cpp/zxing-cpp.git $zxingPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host 'ZXing-C++ already exists.'
}

# Git for Windows may check symbolic links out as tiny text files when
# core.symlinks is disabled. Materialize the bundled zint links so that the
# project builds without requiring Windows Developer Mode.
$linkDirectory = Join-Path $zxingPath 'core\src\libzint'
$zxingFull = [System.IO.Path]::GetFullPath($zxingPath)
foreach ($file in Get-ChildItem -File -Recurse -LiteralPath $linkDirectory) {
    if ($file.Length -gt 256) { continue }
    $relativeTarget = (Get-Content -Raw -LiteralPath $file.FullName).Trim()
    if ($relativeTarget -notmatch '^\.\.[/\\]') { continue }

    $source = [System.IO.Path]::GetFullPath(
        (Join-Path $file.DirectoryName $relativeTarget))
    if (-not $source.StartsWith($zxingFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe bundled zint link target: $source"
    }
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Bundled zint link target is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $file.FullName -Force
}

Write-Host 'ZXing-C++ dependency is ready.'

$opencvConfig = Get-ChildItem -LiteralPath $thirdParty -Recurse -Filter OpenCVConfig.cmake `
    -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $opencvConfig) {
    $opencvPackage = Get-ChildItem -LiteralPath $thirdParty -Filter 'opencv-*-windows.exe' -File `
        | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $opencvPackage) {
        throw 'OpenCV Windows package was not found in third_party. Place opencv-*-windows.exe there.'
    }

    $targetName = $opencvPackage.BaseName -replace '-windows$', ''
    $extractTarget = Join-Path $thirdParty $targetName
    $projectFull = [System.IO.Path]::GetFullPath($projectRoot)
    $targetFull = [System.IO.Path]::GetFullPath($extractTarget)
    if (-not $targetFull.StartsWith($projectFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe OpenCV extraction target: $targetFull"
    }
    if (Test-Path -LiteralPath $extractTarget) {
        throw "OpenCV extraction target exists but is incomplete: $extractTarget"
    }

    New-Item -ItemType Directory -Path $extractTarget | Out-Null
    Write-Host "Extracting $($opencvPackage.Name) to $extractTarget"
    & tar.exe -xf $opencvPackage.FullName -C $extractTarget
    if ($LASTEXITCODE -ne 0) { throw "OpenCV archive extraction failed with code $LASTEXITCODE" }
    $opencvConfig = Get-ChildItem -LiteralPath $extractTarget -Recurse `
        -Filter OpenCVConfig.cmake | Select-Object -First 1
}

if (-not $opencvConfig) {
    throw 'OpenCV extraction completed but OpenCVConfig.cmake was not found.'
}
Write-Host "OpenCV dependency is ready: $($opencvConfig.FullName)"
