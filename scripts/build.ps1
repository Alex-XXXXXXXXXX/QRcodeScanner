$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer was not found: $vswhere"
}
$visualStudioRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $visualStudioRoot) {
    throw 'Visual Studio 2022 with the C++ toolchain was not found.'
}
$cmake = Join-Path $visualStudioRoot `
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$qtRoot = @(
    $env:QT_ROOT,
    'D:\QT\msvc2019_64',
    'C:\Qt\QT5.14.2\5.14.2\msvc2017_64'
) | Where-Object {
    $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin\qmake.exe'))
} | Select-Object -First 1
$buildDir = Join-Path $projectRoot 'build'
$thirdParty = Join-Path $projectRoot 'third_party'

if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake not found: $cmake"
}
if (-not $qtRoot) {
    throw 'Qt 5 MSVC was not found. Set QT_ROOT to the Qt kit directory.'
}

$configureArguments = @(
    '-S', $projectRoot,
    '-B', $buildDir,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$qtRoot",
    '-DQRSCANNER_ENABLE_OPENCV=ON'
)
$opencvConfig = Get-ChildItem -LiteralPath $thirdParty -Recurse -Filter OpenCVConfig.cmake `
    -ErrorAction SilentlyContinue | Select-Object -First 1
if ($opencvConfig) {
    $opencvDir = Split-Path -Parent $opencvConfig.FullName
    $configureArguments += "-DOpenCV_DIR=$opencvDir"
    Write-Host "Using OpenCV: $opencvDir"
} else {
    Write-Warning 'OpenCVConfig.cmake was not found; OpenCV features will be disabled.'
}

& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir --config Debug --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$releaseExe = Join-Path $buildDir 'Release\QRCodeScanner.exe'
$deployTool = Join-Path $qtRoot 'bin\windeployqt.exe'
if ((Test-Path -LiteralPath $releaseExe) -and (Test-Path -LiteralPath $deployTool)) {
    & $deployTool --release --no-translations $releaseExe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($opencvConfig) {
    $opencvBinDirectories = Get-ChildItem -LiteralPath (Split-Path -Parent $opencvConfig.FullName) `
        -Recurse -Directory -Filter bin -ErrorAction SilentlyContinue
    $opencvDlls = foreach ($directory in $opencvBinDirectories) {
        Get-ChildItem -LiteralPath $directory.FullName -Filter 'opencv_world*.dll' -File `
            -ErrorAction SilentlyContinue
    }
    foreach ($dll in $opencvDlls) {
        $isDebug = $dll.BaseName -match 'd$'
        if ($isDebug) {
            $configurationDirectory = 'Debug'
        } else {
            $configurationDirectory = 'Release'
        }
        $destination = Join-Path $buildDir $configurationDirectory
        if (Test-Path -LiteralPath $destination) {
            Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
        }
    }
}

$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$savedPath = $env:PATH
$env:PATH = "$qtRoot\bin;$savedPath"
& $ctest --test-dir $buildDir -C Release --output-on-failure
exit $LASTEXITCODE
