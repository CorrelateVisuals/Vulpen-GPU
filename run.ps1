# Build and run Vulpen: .\run.ps1 [--release] [arguments for vulpen]
$ErrorActionPreference = 'Stop'

$preset = 'debug'
$rest = @($args)
if ($rest.Count -gt 0 -and $rest[0] -eq '--release') { $preset = 'release'; $rest = @($rest | Select-Object -Skip 1) }

# CMake reads the presets from the current folder; vulpen itself runs from the
# caller's, since nothing in it may depend on the working directory.
Push-Location -LiteralPath $PSScriptRoot
try {
    cmake --preset $preset | Out-Null
    if ($LASTEXITCODE -eq 0) { cmake --build --preset $preset }
} finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\out\build\$preset\vulpen.exe" @rest
exit $LASTEXITCODE
