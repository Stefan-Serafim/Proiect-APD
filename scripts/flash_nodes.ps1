param(
    [Parameter(Mandatory)]
    [ValidateSet('A', 'B')]
    [string]$Node,

    [Parameter(Mandatory)]
    [ValidatePattern('^COM[1-9][0-9]*$')]
    [string]$Port
)

if (-not $env:IDF_PATH) {
    throw 'Deschide un terminal ESP-IDF in VS Code.'
}

$idf = Join-Path $env:IDF_PATH 'tools/idf.py'
$projectDir = Split-Path -Parent $PSScriptRoot
$profile = $Node.ToLowerInvariant()

Push-Location $projectDir
try {
    python $idf -B "build/node_$profile" -D "APD_NODE=$profile" -p $Port flash
    if ($LASTEXITCODE -ne 0) {
        throw "Compilarea sau incarcarea a esuat pentru nodul $Node pe $Port."
    }
} finally {
    Pop-Location
}
