[CmdletBinding()]
param(
    [switch]$FrameworkDependent,
    [switch]$BundleToolchain,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'artifacts/launcher' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $output.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Launcher packaging output must be inside this workspace.' }
if ((Test-Path -LiteralPath $output) -and
    (Get-ChildItem -LiteralPath $output -Force | Select-Object -First 1)) {
    throw 'The launcher output is not empty. Select a fresh -OutputDirectory to avoid including existing files.'
}
$env:DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE = 'true'
$env:MSBuildEnableWorkloadResolver = 'false'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_CLI_HOME = Join-Path $root 'build/dotnet-home'
$env:NUGET_PACKAGES = Join-Path $root '.tools/nuget-packages'
$selfContained = if ($FrameworkDependent) { 'false' } else { 'true' }
& dotnet publish (Join-Path $root 'launcher/StuntRaceFX.Launcher.csproj') -c Release -r win-x64 --self-contained $selfContained -o $output -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=None
if ($LASTEXITCODE -ne 0) { throw 'Launcher publish failed.' }
$source = Join-Path $output 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'CMakeLists.txt') -Destination $source
foreach ($directory in @('src', 'include')) {
    $inputDirectory = Join-Path $root $directory
    foreach ($file in Get-ChildItem -LiteralPath $inputDirectory -Recurse -File) {
        if ($file.Extension -notin @('.cpp', '.hpp', '.h', '.inc')) { continue }
        $relative = [IO.Path]::GetRelativePath($root, $file.FullName)
        $target = Join-Path $source $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
}
# BuildPipeline replays these inputs to render previews and validate the local build.
$fixtures = Join-Path $source 'tests/fixtures'
New-Item -ItemType Directory -Path $fixtures -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'tests/fixtures') -Filter '*_inputs.csv' -File |
    Where-Object {
        $_.Name -match '_(clean|coupe|f_type)_inputs\.csv$' -or
        $_.Name -in @('ember_mesa_kestrel_inputs.csv', 'kasumi_loop_cargo_inputs.csv')
    } | Copy-Item -Destination $fixtures

if ($BundleToolchain) {
    $manifestPath = Join-Path $root '.tools/toolchain.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.developmentOnly) { throw 'Cannot package a development-only toolchain. Bootstrap the pinned portable tools first.' }
    $destination = Join-Path $source '.tools'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $manifestPath -Destination $destination
    Copy-Item -LiteralPath (Join-Path $root '.tools/dependencies.json') -Destination $destination
    foreach ($directory in @('llvm-mingw-20250826-ucrt-x86_64', 'cmake-3.31.8-windows-x86_64', 'ninja-1.13.1', 'sdl3-3.4.16', 'openxr-1.1.53')) {
        Copy-Item -LiteralPath (Join-Path $root ".tools/$directory") -Destination $destination -Recurse -Force
    }
} else {
    New-Item -ItemType Directory -Path (Join-Path $source '.tools') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root '.tools/dependencies.json') -Destination (Join-Path $source '.tools') -Force
    # SDL is required by the native source even for external-tool development packages.
    New-Item -ItemType Directory -Path (Join-Path $source '.tools') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root '.tools/sdl3-3.4.16') -Destination (Join-Path $source '.tools') -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $root '.tools/openxr-1.1.53') -Destination (Join-Path $source '.tools') -Recurse -Force
    # Checkout development package uses the local toolchain manifest without copying binaries.
    # It is explicitly labelled development-only, even when the host tools are portable.
    $manifestPath = Join-Path $root '.tools/toolchain.json'
    if (Test-Path -LiteralPath $manifestPath) {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $newDigests = @{}
        foreach ($key in @('compiler', 'cmake', 'ninja')) {
            $old = $manifest.$key
            $resolved = [IO.Path]::GetFullPath($old, (Join-Path $root '.tools'))
            $newDigests[$resolved] = $manifest.fileDigests.$old
            $manifest.$key = $resolved
        }
        $manifest.developmentOnly = $true
        $manifest.name += ' (external tools; development package)'
        $manifest.fileDigests = $newDigests
        New-Item -ItemType Directory -Path (Join-Path $source '.tools') -Force | Out-Null
        $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $source '.tools/toolchain.json') -Encoding utf8
    }
}
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $output
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $output
Write-Host "Launcher package: $output"
Write-Host 'The package contains the native course sources and tools for local ROM extraction.'
