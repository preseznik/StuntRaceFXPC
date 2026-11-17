[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $root '.tools'
$archive = Join-Path $toolsDir 'SDL3-devel-3.4.16-mingw.zip'
$destination = Join-Path $toolsDir 'sdl3-3.4.16'
$digest = '9828bb735cf8a007bcf0ac5aa9f01f3fcb54b7ca67c932e775c905c5d5053a60'
$url = 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-mingw.zip'
New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    $partial = $archive + '.' + [guid]::NewGuid().ToString('N') + '.partial'
    Invoke-WebRequest -Uri $url -OutFile $partial
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ine $digest) {
        throw "SDL download checksum mismatch. File retained at $partial"
    }
    Move-Item -LiteralPath $partial -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $digest) {
    throw 'Cached SDL archive checksum mismatch.'
}
if (-not (Test-Path -LiteralPath $destination)) {
    Expand-Archive -LiteralPath $archive -DestinationPath $destination
}
$sdk = Join-Path $destination 'SDL3-3.4.16/x86_64-w64-mingw32'
foreach ($required in @('bin/SDL3.dll', 'lib/cmake/SDL3/SDL3Config.cmake', 'include/SDL3/SDL_audio.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdk $required) -PathType Leaf)) {
        throw "Incomplete SDL SDK: $required"
    }
}
$manifest = [ordered]@{
    schemaVersion = 1
    sdl = [ordered]@{ version = '3.4.16'; source = $url; archiveSha256 = $digest
        sdk = 'sdl3-3.4.16/SDL3-3.4.16/x86_64-w64-mingw32'
        runtimeSha256 = (Get-FileHash -LiteralPath (Join-Path $sdk 'bin/SDL3.dll') -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$loader = Join-Path $toolsDir 'openxr-1.1.53/lib/libopenxr_loader.a'
if (Test-Path -LiteralPath $loader) {
    $manifest.openxr = [ordered]@{ version = '1.1.53'; commit = '75c53b6e853dc12c7b3c771edc9c9c841b15faaa'
        librarySha256 = (Get-FileHash -LiteralPath $loader -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $toolsDir 'dependencies.json') -Encoding utf8
Write-Host 'SDL3 3.4.16 native audio dependency ready.'
