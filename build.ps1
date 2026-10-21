# build.ps1 <your Mario Kart Wii disc image>
#
# Windows version of build.sh: turns your own PAL disc image into an installable
# PS3 package, out\MKWP00001.pkg, using Docker Desktop.
#
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 "D:\Games\Mario Kart Wii.wbfs"
param([Parameter(Position = 0)][string]$Disc)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$Image = if ($env:MKWPS3_IMAGE) { $env:MKWPS3_IMAGE } else { 'ghcr.io/rooklz/mkwps3-builder:latest' }
$Volume = if ($env:MKWPS3_VOLUME) { $env:MKWPS3_VOLUME } else { 'mkwps3-work' }

if (-not $Disc -or -not (Test-Path -LiteralPath $Disc -PathType Leaf)) {
    Write-Host 'usage: .\build.ps1 "C:\path\to\your\MarioKartWii.wbfs"'
    Write-Host ''
    Write-Host 'Any Wii disc format works (ISO, WBFS, RVZ, WIA, CISO, GCZ). It has to be the'
    Write-Host 'PAL version, RMCP01, and a clean dump. The package lands in out\.'
    exit 1
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "Docker isn't installed. Grab Docker Desktop from https://www.docker.com/products/docker-desktop/, start it, then run this again."
    exit 1
}
docker info *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Docker is installed but not running. Start Docker Desktop, give it a minute, then try again."
    exit 1
}

$DiscItem = Get-Item -LiteralPath $Disc
docker image inspect $Image *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Downloading the builder image (a couple of GB, only the first time)...'
    docker pull $Image
    if ($LASTEXITCODE -ne 0) {
        Write-Host ''
        Write-Host "Couldn't download it, so building it here instead. This compiles the whole PS3"
        Write-Host 'toolchain and takes about an hour, but only ever happens once.'
        docker build -f docker/toolchain.Dockerfile -t mkwps3-toolchain:local .
        if ($LASTEXITCODE -ne 0) { exit 1 }
        docker build --build-arg TOOLCHAIN=mkwps3-toolchain:local -t $Image .
        if ($LASTEXITCODE -ne 0) { exit 1 }
    }
}

New-Item -ItemType Directory -Force -Path out | Out-Null
docker volume create $Volume | Out-Null

docker run --rm -t `
    -v "$($DiscItem.FullName):/game/$($DiscItem.Name):ro" `
    -v "${PWD}\out:/out" `
    -v "${PWD}:/repo:ro" `
    -v "${Volume}:/ws" `
    -e "JOBS=$env:JOBS" `
    $Image "/game/$($DiscItem.Name)" /out
exit $LASTEXITCODE
