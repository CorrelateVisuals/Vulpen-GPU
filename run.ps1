# Build and run Vulpen: .\run.ps1 [--release] [--cli | --ide | --<project> | view.vlp]
#                                  [vulpen's options]
$ErrorActionPreference = 'Stop'

$default = '--ide' # what runs when no view is named; --cli runs the cli alone

$preset = 'debug'
$rest = @($args)
if ($rest.Count -gt 0 -and $rest[0] -eq '--release') { $preset = 'release'; $rest = @($rest | Select-Object -Skip 1) }
$recipes = Join-Path $PSScriptRoot 'src\runtime\recipes'
$projects = Join-Path $PSScriptRoot 'projects'
$options = '--log', '--frames', '--fps', '--first-frame', '--source', '--size', '--restart'
# An app's flag and a project's are the script's own and stand for views, so vulpen knows
# no app or project by name (V05): --cli for the cli's view.vlp, which vulpen starts
# with, and --ide for the cli's and the ide's, which vulpen then hosts beside the
# projects, as it hosts each view named after the first. vulpen's options come before a
# project, so one named like an option runs by its path; each takes a value, and any
# other word is a view.
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
function App($flag) {
    @(Join-Path $recipes 'cli\view.vlp') + @(if ($flag -eq '--ide') { Join-Path $recipes 'ide\view.vlp' })
}
$arguments = @()
$named = $false
$verbose = $false
for ($i = 0; $i -lt $rest.Count; $i++) {
    if ($rest[$i] -in '--cli', '--ide') {
        $arguments += App $rest[$i]; $named = $true
    } elseif ($rest[$i] -in $options) {
        if ($rest[$i] -eq '--log' -and $rest[$i + 1] -eq 'debug') { $verbose = $true }
        $arguments += $rest[$i]
        if ($i + 1 -lt $rest.Count) { $i++; $arguments += $rest[$i] }
    } elseif ($rest[$i] -like '--*') {
        $project = Join-Path $projects "$($rest[$i] -replace '^--')\view.vlp"
        # vulpen names an option it does not know.
        if (Test-Path -LiteralPath $project) { $arguments += $project; $named = $true } else { $arguments += $rest[$i] }
    } else {
        $arguments += $rest[$i]; $named = $true
    }
}
if (-not $named) { $arguments = @(App $default) + $arguments }

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory. One build at a time
# on a tree, as two at once corrupt it: the build folder's build.lock, opened unshared,
# which a running vulpen's builds open so too, and let go before vulpen starts.
$built = Join-Path $PSScriptRoot "out\build\$preset"
New-Item -ItemType Directory -Force -Path $built | Out-Null
$lock = $null
while (-not $lock) {
    try { $lock = [System.IO.File]::Open((Join-Path $built 'build.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch [System.IO.IOException] { Start-Sleep -Milliseconds 100 }
}
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
} finally { Pop-Location; $lock.Dispose() }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\out\build\$preset\vulpen.exe" @arguments
exit $LASTEXITCODE
