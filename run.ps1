# Build and run Vulpen: .\run.ps1 [--release] [arguments for vulpen]
$ErrorActionPreference = 'Stop'

$preset = 'debug'
$rest = @($args)
if ($rest.Count -gt 0 -and $rest[0] -eq '--release') { $preset = 'release'; $rest = @($rest | Select-Object -Skip 1) }
# vulpen's --log sets how much the build says too: all of it at debug, and at any other
# level nothing unless it fails (C09).
$verbose = $false
for ($i = 1; $i -lt $rest.Count; $i++) {
    if ($rest[$i - 1] -eq '--log' -and $rest[$i] -eq 'debug') { $verbose = $true }
}

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

& "$PSScriptRoot\out\build\$preset\vulpen.exe" @rest
exit $LASTEXITCODE
