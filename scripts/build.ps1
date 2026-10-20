<#
.SYNOPSIS
    Configura, compila y (opcionalmente) prueba MovixStudio con MSVC + Ninja.
.EXAMPLE
    ./scripts/build.ps1                 # Debug
    ./scripts/build.ps1 -Preset release -Test
    ./scripts/build.ps1 -Preset release -Package   # instalador MSI y ZIP portable en build/release
#>
param(
    [ValidateSet('debug', 'release')] [string] $Preset = 'debug',
    [switch] $Test,
    [switch] $Package,
    [switch] $Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No se encontró Visual Studio / Build Tools con las herramientas de C++.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

if ($Clean) { Remove-Item -Recurse -Force (Join-Path $root "build\$Preset") -ErrorAction SilentlyContinue }

$steps = @(
    "cmake --preset $Preset",
    "cmake --build --preset $Preset"
)
if ($Test) { $steps += "ctest --preset $Preset" }
if ($Package) { $steps += "cd build\$Preset && cpack -C $(if ($Preset -eq 'debug') { 'Debug' } else { 'RelWithDebInfo' })" }

$cmd = "`"$vcvars`" >nul && cd /d `"$root`" && " + ($steps -join ' && ')
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "La compilación falló (código $LASTEXITCODE)." }
