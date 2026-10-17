<#
.SYNOPSIS
    Ejecuta MovixStudio desde el directorio de compilación (añade Qt al PATH).
.EXAMPLE
    ./scripts/run.ps1
    ./scripts/run.ps1 -Preset release -- "C:\Videos\clip.mp4"
#>
param(
    [ValidateSet('debug', 'release')] [string] $Preset = 'debug',
    [Parameter(ValueFromRemainingArguments)] [string[]] $Arguments
)
$root = Split-Path -Parent $PSScriptRoot
$cache = Get-Content (Join-Path $root "build\$Preset\CMakeCache.txt")
$qtRoot = ($cache | Select-String '^CMAKE_PREFIX_PATH[^=]*=(.*)$').Matches[0].Groups[1].Value
$env:PATH = "$qtRoot\bin;$env:PATH"
& (Join-Path $root "build\$Preset\MovixStudio.exe") @Arguments
