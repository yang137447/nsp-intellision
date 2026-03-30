Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$serverBuildDir = Join-Path $repoRoot "server_cpp\build"

function Invoke-GateStep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host "[D3 Gate] $Label"
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "[D3 Gate] $Label failed with exit code $LASTEXITCODE"
    }
}

Invoke-GateStep "json validate" { npm run json:validate }
Invoke-GateStep "compile" { npm run compile }
Invoke-GateStep "build server_cpp" { cmake --build $serverBuildDir }
Invoke-GateStep "hover smoke" { python "$repoRoot\server_cpp\tools\hover_smoke_test.py" }
Invoke-GateStep "client all tests" { npm run test:client:all }

Write-Host "[D3 Gate] passed"
