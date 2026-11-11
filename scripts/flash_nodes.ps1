param(
    [Parameter(Mandatory)]
    [ValidateSet('A', 'B')]
    [string]$Node,

    [Parameter(Mandatory)]
    [ValidatePattern('^COM[1-9][0-9]*$')]
    [string]$Port
)

if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
    throw 'Deschide un terminal PlatformIO in VS Code.'
}

$projectDir = Split-Path -Parent $PSScriptRoot
$environment = "node_$($Node.ToLowerInvariant())"

pio run -d $projectDir -e $environment -t upload --upload-port $Port
if ($LASTEXITCODE -ne 0) {
    throw "Compilarea sau incarcarea firmware-ului a esuat pentru $Node pe $Port."
}

pio run -d $projectDir -e $environment -t uploadfs --upload-port $Port
if ($LASTEXITCODE -ne 0) {
    throw "Incarcarea LittleFS a esuat pentru $Node pe $Port."
}
