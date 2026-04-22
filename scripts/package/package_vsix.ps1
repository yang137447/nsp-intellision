param(
	[string]$OutFile = "",
	[switch]$KeepStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
. (Join-Path $repoRoot "scripts\common\server_build.ps1")
$packageJsonPath = Join-Path $repoRoot "package.json"
$packageJson = Get-Content -Raw -Encoding utf8 -Path $packageJsonPath | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($OutFile)) {
	$OutFile = "$($packageJson.name)-$($packageJson.version).vsix"
}

$stageRoot = Join-Path $repoRoot ".vsix-stage"
$stagePackageJsonPath = Join-Path $stageRoot "package.json"
$cleanBuildDir = Join-Path $repoRoot "server_cpp\build_vsix"
$outputPath = Join-Path $repoRoot $OutFile

function Ensure-Directory([string]$Path) {
	New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Copy-ClientRuntimePackage([string]$PackageName, [hashtable]$Seen) {
	if ($Seen.ContainsKey($PackageName)) {
		return
	}

	$source = Join-Path $repoRoot "client\node_modules\$PackageName"
	if (-not (Test-Path $source)) {
		throw "Missing client runtime dependency: $PackageName"
	}

	$targetParent = Join-Path $stageRoot "client\node_modules"
	Ensure-Directory $targetParent
	Copy-Item $source (Join-Path $targetParent $PackageName) -Recurse -Force
	$Seen[$PackageName] = $true

	$depPackageJsonPath = Join-Path $source "package.json"
	if (-not (Test-Path $depPackageJsonPath)) {
		return
	}

	$depPackageJson = Get-Content -Raw -Encoding utf8 -Path $depPackageJsonPath | ConvertFrom-Json
	if (-not ($depPackageJson.PSObject.Properties.Name -contains "dependencies")) {
		return
	}
	if (-not $depPackageJson.dependencies) {
		return
	}

	foreach ($dep in $depPackageJson.dependencies.PSObject.Properties.Name) {
		Copy-ClientRuntimePackage $dep $Seen
	}
}

function Remove-IfExists([string]$Path) {
	if (Test-Path $Path) {
		Remove-Item -Recurse -Force $Path
	}
}

Write-Host "[package:vsix] compile"
npm run compile

Write-Host "[package:vsix] configure clean server build"
$serverBuild = Initialize-CleanServerBuild $repoRoot "server_cpp\build_vsix" "Release"

Write-Host "[package:vsix] build clean server"
Invoke-ServerBuild $serverBuild.BuildDir 4

Write-Host "[package:vsix] stage package contents"
Reset-Directory $stageRoot

foreach ($dir in @(
	"client",
	"client\out",
	"client\node_modules",
	"server_cpp",
	"server_cpp\build",
	"server_cpp\build\resources",
	"syntaxes",
	"snippets"
)) {
	Ensure-Directory (Join-Path $stageRoot $dir)
}

Copy-Item $packageJsonPath $stagePackageJsonPath -Force
Copy-Item (Join-Path $repoRoot "README.md") (Join-Path $stageRoot "README.md") -Force
Copy-Item (Join-Path $repoRoot "LICENSE") (Join-Path $stageRoot "LICENSE") -Force
Copy-Item (Join-Path $repoRoot "syntaxes\*") (Join-Path $stageRoot "syntaxes") -Recurse -Force
Copy-Item (Join-Path $repoRoot "snippets\*") (Join-Path $stageRoot "snippets") -Recurse -Force
Copy-Item (Join-Path $cleanBuildDir "nsf_lsp.exe") (Join-Path $stageRoot "server_cpp\build\nsf_lsp.exe") -Force
Copy-Item (Join-Path $cleanBuildDir "resources\*") (Join-Path $stageRoot "server_cpp\build\resources") -Recurse -Force
Copy-Item (Join-Path $repoRoot "client\out\*.js") (Join-Path $stageRoot "client\out") -Force

$seenPackages = @{}
Copy-ClientRuntimePackage "vscode-languageclient" $seenPackages

$nodeModulesRoot = Join-Path $stageRoot "client\node_modules"
foreach ($pattern in @("*.d.ts", "*.ts", "*.map", "*.md", "*.markdown")) {
	Get-ChildItem -Path $nodeModulesRoot -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
		Remove-Item -Force
}

Get-ChildItem -Path $nodeModulesRoot -Recurse -File -ErrorAction SilentlyContinue |
	Where-Object {
		$_.Name -eq ".travis.yml" -or
		$_.Name -eq ".editorconfig" -or
		$_.Name -like ".eslintrc*" -or
		$_.Name -like "CHANGELOG*" -or
		$_.Name -like "tsconfig*.json"
	} |
	Remove-Item -Force

Get-ChildItem -Path $nodeModulesRoot -Recurse -Directory -ErrorAction SilentlyContinue |
	Where-Object {
		$_.Name -in @("test", "tests", "example", "examples", ".github")
	} |
	Sort-Object FullName -Descending |
	ForEach-Object { Remove-Item -Recurse -Force $_.FullName }

$stagePackageJson = Get-Content -Raw -Encoding utf8 -Path $stagePackageJsonPath | ConvertFrom-Json
if ($stagePackageJson.PSObject.Properties.Name -contains "files") {
	$stagePackageJson.PSObject.Properties.Remove("files")
}
$stagePackageJson.scripts."vscode:prepublish" = "echo skip prepublish in staging"
$stagePackageJsonText = $stagePackageJson | ConvertTo-Json -Depth 100
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($stagePackageJsonPath, $stagePackageJsonText, $utf8NoBom)

Write-Host "[package:vsix] package"
Remove-IfExists $outputPath
Push-Location $stageRoot
try {
	npx vsce package --no-dependencies -o "..\$OutFile"
} finally {
	Pop-Location
}

if (-not (Test-Path $outputPath)) {
	throw "Package output not found: $outputPath"
}

Write-Host "[package:vsix] packaged $outputPath"

if (-not $KeepStage) {
	Remove-IfExists $stageRoot
}
