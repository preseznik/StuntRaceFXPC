# Build the static loader with the same C++ runtime as the native game.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Compiler,[Parameter(Mandatory)][string]$CMake,[Parameter(Mandatory)][string]$Ninja)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolDir = Join-Path $root '.tools'
$out = Join-Path $toolDir 'openxr-1.1.53'
$build = Join-Path $root 'build/openxr-sdk'
$sdk = Join-Path $toolDir 'downloads/OpenXR-SDK-1.1.53'
$commit = '75c53b6e853dc12c7b3c771edc9c9c841b15faaa'
if (!(Test-Path -LiteralPath $sdk)) {
    git clone --depth 1 --branch release-1.1.53 https://github.com/KhronosGroup/OpenXR-SDK.git $sdk
    if ($LASTEXITCODE) { throw 'Could not obtain the OpenXR SDK.' }
}
$revision = git -C $sdk rev-parse HEAD
if ($LASTEXITCODE -or $revision -cne $commit) { throw 'OpenXR SDK revision mismatch.' }
$changes = git -C $sdk status --porcelain --untracked-files=normal
if ($LASTEXITCODE -or $changes) { throw 'OpenXR dependency checkout has local changes.' }
$cxx = [IO.Path]::GetFullPath($Compiler,$toolDir).Replace('\','/')
$cc = $cxx.Replace('clang++.exe','clang.exe').Replace('g++.exe','gcc.exe')
$cm = [IO.Path]::GetFullPath($CMake,$toolDir)
$nj = [IO.Path]::GetFullPath($Ninja,$toolDir).Replace('\','/')
$rc = if ($cxx.EndsWith('clang++.exe')) {'llvm-windres.exe'} else {'windres.exe'}
$rc = (Join-Path (Split-Path $cxx) $rc).Replace('\','/')
& $cm -S $sdk -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$nj" "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx" "-DCMAKE_RC_COMPILER=$rc" -DCMAKE_BUILD_TYPE=Release -DDYNAMIC_LOADER=OFF -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF -DBUILD_WITH_SYSTEM_JSONCPP=OFF
if ($LASTEXITCODE) { throw 'OpenXR configuration failed.' }
& $cm --build $build --target openxr_loader --parallel 4
if ($LASTEXITCODE) { throw 'OpenXR loader compilation failed.' }
New-Item -ItemType Directory -Force "$out/include/openxr","$out/lib" | Out-Null
Copy-Item -Path "$sdk/include/openxr/openxr*.h" -Destination "$out/include/openxr" -Force
Copy-Item -LiteralPath "$build/src/loader/libopenxr_loader.a" -Destination "$out/lib" -Force
Copy-Item -LiteralPath "$sdk/LICENSE","$sdk/COPYING.adoc" -Destination $out -Force
New-Item -ItemType Directory -Force "$out/LICENSES" | Out-Null
Copy-Item -Path "$sdk/LICENSES/*" -Destination "$out/LICENSES" -Force
Copy-Item -LiteralPath "$sdk/src/external/jsoncpp/LICENSE" -Destination "$out/jsoncpp-LICENSE" -Force
