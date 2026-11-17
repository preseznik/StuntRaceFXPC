[CmdletBinding()]
param(
    [switch]$UseDevelopmentTools,
    [string]$DevelopmentBin = 'C:\Strawberry\c\bin'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $root '.tools'
New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null

if ($UseDevelopmentTools) {
    $compiler = Join-Path $DevelopmentBin 'g++.exe'
    $cmake = Join-Path $DevelopmentBin 'cmake.exe'
    $ninja = Join-Path $DevelopmentBin 'ninja.exe'
    foreach ($executable in @($compiler, $cmake, $ninja)) {
        if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing development tool: $executable" }
    }
    $name = 'Installed GCC/CMake/Ninja (development only; not portable)'
    Write-Warning $name
} else {
    # SHA256 pins recorded from the official GitHub release asset metadata.
    $packages = @(
        @{ Name='LLVM-MinGW 20250826 UCRT x64'; File='llvm-mingw-20250826-ucrt-x86_64.zip'; Directory='llvm-mingw-20250826-ucrt-x86_64'; Url='https://github.com/mstorsjo/llvm-mingw/releases/download/20250826/llvm-mingw-20250826-ucrt-x86_64.zip'; Sha256='51c347bf5dead6c4383ff16fa9baee362dacceb70ecda9448d2289c8e158af61'; Rooted=$true },
        @{ Name='CMake 3.31.8'; File='cmake-3.31.8-windows-x86_64.zip'; Directory='cmake-3.31.8-windows-x86_64'; Url='https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip'; Sha256='81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'; Rooted=$true },
        @{ Name='Ninja 1.13.1'; File='ninja-1.13.1-win.zip'; Directory='ninja-1.13.1'; Url='https://github.com/ninja-build/ninja/releases/download/v1.13.1/ninja-win.zip'; Sha256='26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf'; Rooted=$false }
    )
    $downloads = Join-Path $toolsDir 'downloads'
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    foreach ($package in $packages) {
        $archive = Join-Path $downloads $package.File
        if (-not (Test-Path -LiteralPath $archive)) {
            Write-Host "Downloading $($package.Name)..."
            $partial = $archive + '.' + [guid]::NewGuid().ToString('N') + '.partial'
            Invoke-WebRequest -Uri $package.Url -OutFile $partial
            if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ine $package.Sha256) { throw "Download checksum mismatch: $($package.Name). Untrusted file retained at $partial" }
            Move-Item -LiteralPath $partial -Destination $archive
        }
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $package.Sha256) { throw "Cached archive checksum mismatch: $archive" }
        $destination = Join-Path $toolsDir $package.Directory
        if (-not (Test-Path -LiteralPath $destination)) {
            $unpack = Join-Path $toolsDir ('unpack-' + [guid]::NewGuid().ToString('N'))
            Write-Host "Extracting $($package.Name)..."
            Expand-Archive -LiteralPath $archive -DestinationPath $unpack
            $extracted = if ($package.Rooted) { Join-Path $unpack $package.Directory } else { $unpack }
            foreach ($movePath in @($extracted, $destination)) {
                $absolute = [IO.Path]::GetFullPath($movePath)
                if (-not $absolute.StartsWith([IO.Path]::GetFullPath($toolsDir) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Refusing to move a directory outside the project tool cache: $absolute"
                }
            }
            Move-Item -LiteralPath $extracted -Destination $destination
        }
    }
    $compiler = 'llvm-mingw-20250826-ucrt-x86_64/bin/clang++.exe'
    $cmake = 'cmake-3.31.8-windows-x86_64/bin/cmake.exe'
    $ninja = 'ninja-1.13.1/ninja.exe'
    $name = 'LLVM-MinGW 20250826 UCRT x64 / CMake 3.31.8 / Ninja 1.13.1'
}

& (Join-Path $PSScriptRoot 'Bootstrap-OpenXR.ps1') -Compiler $compiler -CMake $cmake -Ninja $ninja
& (Join-Path $PSScriptRoot 'Bootstrap-Dependencies.ps1')
$digests = [ordered]@{}
foreach ($executable in @($compiler, $cmake, $ninja)) {
    $resolved = if ([IO.Path]::IsPathRooted($executable)) { $executable } else { Join-Path $toolsDir $executable }
    $digests[$executable] = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    $versionOutput = & $resolved --version
    if ($LASTEXITCODE -ne 0) { throw "Cannot run tool: $resolved" }
    $versionOutput | Select-Object -First 1 | Write-Host
}
$manifest = [ordered]@{
    schemaVersion = 1
    name = $name
    developmentOnly = [bool]$UseDevelopmentTools
    compiler = $compiler
    cmake = $cmake
    ninja = $ninja
    fileDigests = $digests
}
$temporary = Join-Path $toolsDir ('toolchain-' + [guid]::NewGuid().ToString('N') + '.tmp')
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporary -Encoding utf8
Move-Item -LiteralPath $temporary -Destination (Join-Path $toolsDir 'toolchain.json') -Force
Write-Host "Toolchain ready: $name"
