$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build-vscode'
$qmake = 'D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\qmake.exe'
$make = 'D:\Qt\Qt5.14.2\Tools\mingw730_64\bin\mingw32-make.exe'
$qtBin = Split-Path -Parent $qmake
$mingwBin = Split-Path -Parent $make

# qmake's Makefile invokes g++/windres by command name. Put Qt's MinGW first
# so unrelated toolchains (for example Strawberry Perl) cannot be selected.
$env:Path = "$mingwBin;$qtBin;$env:Path"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
Push-Location $buildDir
try {
    & $qmake (Join-Path $projectRoot 'SLAM_META_Tool.pro') 'CONFIG+=debug' 'CONFIG-=release'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $make -j4
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}

