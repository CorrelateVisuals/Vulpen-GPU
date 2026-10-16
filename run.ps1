# Build and run Vulpen: .\run.ps1 [--release] [--cli | --ide | view.vlp] [vulpen's options]
$ErrorActionPreference = 'Stop'

$default = '--ide' # what runs when no view is named; --cli runs the cli alone

$preset = 'debug'
$rest = @($args)
if ($rest.Count -gt 0 -and $rest[0] -eq '--release') { $preset = 'release'; $rest = @($rest | Select-Object -Skip 1) }
$recipes = Join-Path $PSScriptRoot 'src\runtime\recipes'
# An app's flag is the script's own and stands for the app's view.vlp, so vulpen knows no
# app by name (V05). Each of vulpen's options takes a value, so any other word is a view.
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
$arguments = @()
$named = $false
$verbose = $false
for ($i = 0; $i -lt $rest.Count; $i++) {
    if ($rest[$i] -in '--cli', '--ide') {
        $arguments += Join-Path $recipes "$($rest[$i] -replace '^--')\view.vlp"; $named = $true
    } elseif ($rest[$i] -like '--*') {
        if ($rest[$i] -eq '--log' -and $rest[$i + 1] -eq 'debug') { $verbose = $true }
        $arguments += $rest[$i]
        if ($i + 1 -lt $rest.Count) { $i++; $arguments += $rest[$i] }
    } else {
        $arguments += $rest[$i]; $named = $true
    }
}
if (-not $named) { $arguments = @(Join-Path $recipes "$($default -replace '^--')\view.vlp") + $arguments }

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory.
Push-Location -LiteralPath $PSScriptRoot
try {
    # Continue: a native command's stderr is part of what the build said, not a failure.
    $ErrorActionPreference = 'Continue'
    $said = & {
        cmake --preset $preset
        if ($LASTEXITCODE -eq 0) { cmake --build --preset $preset --parallel }
    } 2>&1 | ForEach-Object { if ($verbose) { "$_" | Out-Host } else { "$_" } }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) { $said | Out-Host }
} finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\out\build\$preset\vulpen.exe" @arguments
exit $LASTEXITCODE
