Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

Write-Host "[D3 Gate] json validate"
npm run json:validate

Write-Host "[D3 Gate] compile"
npm run compile

Write-Host "[D3 Gate] build server_cpp"
cmake --build "$repoRoot\server_cpp\build_mingw"

Write-Host "[D3 Gate] hover smoke"
python "$repoRoot\server_cpp\tools\hover_smoke_test.py"

Write-Host "[D3 Gate] client all tests"
npm run test:client:all

Write-Host "[D3 Gate] passed"
