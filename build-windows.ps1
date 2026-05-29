# build-windows.ps1
# Lokalny skrypt do budowania Lape's Eye na Windows 11
# Uruchom w PowerShell jako administrator lub z dostępem do MSVC
#
# Wymagania wstępne (zainstaluj raz):
#   1. Visual Studio 2022 Build Tools (workload: "Desktop development with C++")
#   2. Qt 6.7 dla MSVC 2019 x64 — https://www.qt.io/download-open-source
#   3. CMake >= 3.20 — https://cmake.org/download/
#   4. Git — https://git-scm.com/
#   5. NSIS 3.x — https://nsis.sourceforge.io/ (tylko do budowania instalatora)

param(
    [string]$QtDir     = "C:\Qt\6.7.3\msvc2019_64",
    [string]$VcpkgDir  = "C:\vcpkg",
    [string]$BuildType = "Release",
    [switch]$Installer,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $ScriptDir "build-win"
$DeployDir = Join-Path $ScriptDir "deploy-win"

Write-Host "=== Lape's Eye — Windows Build ===" -ForegroundColor Cyan
Write-Host "Qt:     $QtDir"
Write-Host "vcpkg:  $VcpkgDir"
Write-Host "Typ:    $BuildType"
Write-Host ""

# ── Sprawdź vcpkg ─────────────────────────────────────────────────────────
if (!(Test-Path "$VcpkgDir\vcpkg.exe")) {
    Write-Host "Klonowanie vcpkg..." -ForegroundColor Yellow
    git clone https://github.com/microsoft/vcpkg.git $VcpkgDir
    & "$VcpkgDir\bootstrap-vcpkg.bat" -disableMetrics
}

# ── Zainstaluj zależności przez vcpkg ─────────────────────────────────────
Write-Host "Instalowanie zależności (exiv2, libraw)..." -ForegroundColor Yellow
& "$VcpkgDir\vcpkg.exe" install exiv2:x64-windows libraw:x64-windows --recurse
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }

# ── Wyczyść poprzedni build ───────────────────────────────────────────────
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Czyszczenie $BuildDir..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

# ── Konfiguracja CMake ────────────────────────────────────────────────────
Write-Host "Konfiguracja CMake..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force $BuildDir | Out-Null

$cmakeArgs = @(
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgDir\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DCMAKE_PREFIX_PATH=$QtDir",
    "-DCMAKE_INSTALL_PREFIX=$DeployDir"
)
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ── Kompilacja ────────────────────────────────────────────────────────────
Write-Host "Kompilacja..." -ForegroundColor Yellow
& cmake --build $BuildDir --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# ── Install + windeployqt ─────────────────────────────────────────────────
Write-Host "Deploy..." -ForegroundColor Yellow
& cmake --install $BuildDir --prefix $DeployDir
if ($LASTEXITCODE -ne 0) { throw "Install failed" }

$windeployqt = Join-Path $QtDir "bin\windeployqt.exe"
& $windeployqt --release --no-translations --no-system-d3d-compiler `
    (Join-Path $DeployDir "bin\lapes-eye.exe")

# ── Kopiuj DLL-e z vcpkg ──────────────────────────────────────────────────
$vcpkgBin = "$VcpkgDir\installed\x64-windows\bin"
$destBin   = "$DeployDir\bin"
Get-ChildItem "$vcpkgBin\*.dll" | ForEach-Object {
    $dest = Join-Path $destBin $_.Name
    if (!(Test-Path $dest)) {
        Copy-Item $_.FullName $dest
        Write-Host "  + $($_.Name)"
    }
}

Write-Host ""
Write-Host "=== Build gotowy: $DeployDir ===" -ForegroundColor Green

# ── NSIS Installer (opcjonalnie) ──────────────────────────────────────────
if ($Installer) {
    Write-Host "Budowanie instalatora NSIS..." -ForegroundColor Yellow

    $makensis = Get-Command makensis -ErrorAction SilentlyContinue
    if (!$makensis) {
        $makensis = "C:\Program Files (x86)\NSIS\makensis.exe"
        if (!(Test-Path $makensis)) {
            Write-Warning "NSIS nie znaleziony. Zainstaluj z https://nsis.sourceforge.io/"
            exit 0
        }
    }

    # Pobierz wersję z CMakeLists.txt
    $version = (Select-String -Path "$ScriptDir\CMakeLists.txt" `
        -Pattern 'project\(LapesEye VERSION ([0-9.]+)').Matches[0].Groups[1].Value

    $nsiScript = Join-Path $ScriptDir "installer\lapes-eye-installer.nsi"
    & $makensis /V2 `
        "/DAPP_VERSION=$version" `
        "/DDEPLOY_DIR=$DeployDir" `
        $nsiScript

    if ($LASTEXITCODE -eq 0) {
        $installerPath = Join-Path (Split-Path $nsiScript) "lapes-eye-setup-$version.exe"
        Write-Host "=== Instalator: $installerPath ===" -ForegroundColor Green
    } else {
        throw "NSIS build failed"
    }
}
