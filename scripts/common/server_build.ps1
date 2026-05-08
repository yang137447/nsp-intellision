Set-StrictMode -Version Latest

function Reset-Directory([string]$Path) {
    if (Test-Path $Path) {
        Remove-Item -Recurse -Force $Path
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Add-UniquePath([System.Collections.Generic.List[string]]$Paths, [string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    if (-not (Test-Path $Path)) {
        return
    }
    $resolvedPath = (Resolve-Path $Path).Path
    if (-not $Paths.Contains($resolvedPath)) {
        $Paths.Add($resolvedPath)
    }
}

function Get-ClangVersionInfo([string]$CompilerPath) {
    if ([string]::IsNullOrWhiteSpace($CompilerPath)) {
        return $null
    }
    if (-not (Test-Path $CompilerPath)) {
        return $null
    }

    $firstLine = & $CompilerPath --version 2>$null | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($firstLine)) {
        return $null
    }

    $match = [regex]::Match($firstLine, "clang version (?<major>\d+)\.(?<minor>\d+)\.(?<patch>\d+)")
    if (-not $match.Success) {
        return $null
    }

    return [pscustomobject]@{
        Path = (Resolve-Path $CompilerPath).Path
        Major = [int]$match.Groups["major"].Value
        Minor = [int]$match.Groups["minor"].Value
        Patch = [int]$match.Groups["patch"].Value
        Version = "$($match.Groups["major"].Value).$($match.Groups["minor"].Value).$($match.Groups["patch"].Value)"
        Banner = $firstLine
    }
}

function Get-BuildCompilerCandidates() {
    $candidates = New-Object "System.Collections.Generic.List[string]"

    if (-not [string]::IsNullOrWhiteSpace($env:NSF_PACKAGE_CXX_COMPILER)) {
        Add-UniquePath $candidates $env:NSF_PACKAGE_CXX_COMPILER
    }

    Add-UniquePath $candidates "C:\Software\llvm-mingw\bin\clang++.exe"

    foreach ($llvmMingwDir in (Get-ChildItem -Path "C:\Software" -Directory -Filter "llvm-mingw-*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {
        Add-UniquePath $candidates (Join-Path $llvmMingwDir.FullName "bin\clang++.exe")
    }

    try {
        foreach ($pathEntry in (& where.exe clang++ 2>$null)) {
            Add-UniquePath $candidates $pathEntry
        }
    } catch {
    }

    return $candidates
}

function Resolve-BuildCompiler() {
    $candidates = Get-BuildCompilerCandidates
    $rejected = @()

    if (-not [string]::IsNullOrWhiteSpace($env:NSF_PACKAGE_CXX_COMPILER)) {
        $overrideInfo = Get-ClangVersionInfo $env:NSF_PACKAGE_CXX_COMPILER
        if ($null -eq $overrideInfo) {
            throw "NSF_PACKAGE_CXX_COMPILER is set but does not point to a usable clang++ executable: $($env:NSF_PACKAGE_CXX_COMPILER)"
        }
        if ($overrideInfo.Major -lt 20) {
            throw "NSF_PACKAGE_CXX_COMPILER requires Clang 20 or newer, got $($overrideInfo.Version) at $($overrideInfo.Path)"
        }
        return $overrideInfo
    }

    foreach ($candidate in $candidates) {
        $info = Get-ClangVersionInfo $candidate
        if ($null -eq $info) {
            continue
        }
        if ($info.Major -ge 20) {
            return $info
        }
        $rejected += "$($info.Path) -> Clang $($info.Version)"
    }

    if ($rejected.Count -gt 0) {
        throw "No supported clang++ compiler found. Need Clang 20 or newer. Rejected candidates: $($rejected -join '; '). You can override with NSF_PACKAGE_CXX_COMPILER=<full-path-to-clang++.exe>."
    }

    throw "No clang++ compiler found. Install llvm-mingw Clang 20+ or set NSF_PACKAGE_CXX_COMPILER=<full-path-to-clang++.exe>."
}

function Initialize-CleanServerBuild([string]$RepoRoot, [string]$BuildDirRelativePath, [string]$BuildType = "Release") {
    $buildDir = Join-Path $RepoRoot $BuildDirRelativePath
    $buildCompiler = Resolve-BuildCompiler

    Write-Host "[server build] compiler $($buildCompiler.Path) (Clang $($buildCompiler.Version))"
    Write-Host "[server build] configure clean build $buildDir"

    Reset-Directory $buildDir
    cmake -S (Join-Path $RepoRoot "server_cpp") -B $buildDir -G "MinGW Makefiles" -D CMAKE_BUILD_TYPE=$BuildType -D CMAKE_CXX_COMPILER="$($buildCompiler.Path)" | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to configure server build at $buildDir"
    }

    return [pscustomobject]@{
        BuildDir = $buildDir
        Compiler = $buildCompiler
        BuildType = $BuildType
    }
}

function Invoke-ServerBuild([string]$BuildDir, [int]$Jobs = 4) {
    Push-Location $BuildDir
    try {
        cmake --build . -j $Jobs | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build server at $BuildDir"
        }
    } finally {
        Pop-Location
    }
}
