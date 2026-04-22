Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$serverBuildDir = Join-Path $repoRoot "server_cpp\build"
. (Join-Path $repoRoot "scripts\common\server_build.ps1")

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
Invoke-GateStep "configure clean server_cpp" { Initialize-CleanServerBuild $repoRoot "server_cpp\build" "Release" | Out-Null }
Invoke-GateStep "build server_cpp" { Invoke-ServerBuild $serverBuildDir 4 }
Invoke-GateStep "hover smoke" { py -3 "$repoRoot\server_cpp\tools\hover_smoke_test.py" }
Invoke-GateStep "client all tests" { npm run test:client:all }

Write-Host "[D3 Gate] passed"
