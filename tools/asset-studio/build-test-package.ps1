<#
.SYNOPSIS
    Builds Ferrum56 and bakes the complete retail Half-Life asset set into a
    standalone, runnable folder (ferrum56-build-test by default).

.DESCRIPTION
    The engine resolves every archive relative to its working directory
    (ferrum-engine/src/main.rs: "global.mpak", "menu.mpak", "menu_bg.mpak",
    "<map>.mpak"), so "a runnable build" is really "a directory holding the
    executable next to a complete set of .mpak files plus the handful of loose
    files the engine still reads directly from disk". This script produces
    exactly that directory, and is safe to re-run after every code change -
    the engine is always recopied, while map archives are only rebaked when
    their source .bsp is newer (or -Force is passed).

    Asset layout produced here deliberately differs from the Xbox
    "monolithic packaging" model described in .agents/AGENTS.md: the whole
    retail sound tree (~65 MB, 2500+ files) goes into global.mpak ONCE
    instead of being duplicated into all ~125 map archives, which would cost
    several GB and dominate bake time. Per-map sound duplication only earns
    its keep on the Xbox's seek-bound IDE/DVD path; a PC test build wants the
    shared archive. Map archives still carry their own textures, lightmaps
    and entity-scraped models, so the per-map bake is otherwise unchanged.

.PARAMETER GameDir
    A retail Half-Life "valve" directory - the source of every asset.

.PARAMETER OutDir
    Destination folder. Created if missing.

.PARAMETER BuildProfile
    Cargo profile for the engine build: release (default) or debug.

.PARAMETER Maps
    Optional subset of map names (no extension) to bake instead of all of
    them, e.g. -Maps c0a0,c1a0,c1a1. Handy for a quick turnaround.

.PARAMETER Force
    Rebake every archive even when it looks up to date.

.PARAMETER SkipAssets
    Only rebuild and recopy the engine executable. This is the fast path for
    "I changed Rust code, refresh the runnable folder".

.EXAMPLE
    ./scripts/build-test-package.ps1
    Full build: engine + every retail map + global/menu archives.

.EXAMPLE
    ./scripts/build-test-package.ps1 -SkipAssets
    Refresh just the executable after a code change.
#>
[CmdletBinding()]
param(
    [string]$GameDir = "D:\SteamLibrary\steamapps\common\Half-Life\valve",
    [string]$OutDir = (Join-Path $PSScriptRoot "..\ferrum56-build-test"),
    [ValidateSet("release", "debug")][string]$BuildProfile = "release",
    [string[]]$Maps,
    [switch]$Force,
    [switch]$SkipAssets
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Write-Step($message) {
    Write-Host ""
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Write-Warn($message) {
    Write-Host "    warning: $message" -ForegroundColor Yellow
}

# Runs a native executable with the exit code, not stderr, as the success
# signal. Windows PowerShell wraps every stderr line from a native process in
# an ErrorRecord, so under $ErrorActionPreference = "Stop" a single cargo
# warning would abort the whole package - even though cargo exited 0.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][scriptblock]$Command,
        [Parameter(Mandatory)][string]$What,
        [switch]$Quiet
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        if ($Quiet) { & $Command *> $null } else { & $Command }
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

# ---------------------------------------------------------------------------
# 0. Validate inputs before doing any expensive work
# ---------------------------------------------------------------------------

if (-not (Test-Path $GameDir)) {
    throw "Game directory not found: $GameDir. Pass -GameDir <path to a retail Half-Life 'valve' folder>."
}
foreach ($required in @("maps", "sound", "halflife.wad")) {
    if (-not (Test-Path (Join-Path $GameDir $required))) {
        throw "'$GameDir' does not look like a retail valve directory - missing '$required'."
    }
}

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}
$OutDir = (Resolve-Path $OutDir).Path
Write-Host "Ferrum56 build-test packager"
Write-Host "  game dir : $GameDir"
Write-Host "  out dir  : $OutDir"
Write-Host "  profile  : $BuildProfile"

# ---------------------------------------------------------------------------
# 1. Engine
# ---------------------------------------------------------------------------

Write-Step "Building the engine ($BuildProfile)"
Push-Location $RepoRoot
try {
    if ($BuildProfile -eq "release") {
        Invoke-Native -What "cargo build --release" -Command { cargo build --release -p ferrum-engine }
    } else {
        Invoke-Native -What "cargo build" -Command { cargo build -p ferrum-engine }
    }
} finally {
    Pop-Location
}

$EngineExe = Join-Path $RepoRoot "target\$BuildProfile\ferrum-engine.exe"
if (-not (Test-Path $EngineExe)) { throw "engine executable not found after build: $EngineExe" }
Copy-Item $EngineExe (Join-Path $OutDir "ferrum-engine.exe") -Force
Write-Host "    copied ferrum-engine.exe ($([math]::Round((Get-Item $EngineExe).Length / 1MB, 1)) MB)"

if ($SkipAssets) {
    Write-Step "-SkipAssets given - stopping after the engine refresh"
    exit 0
}

# ---------------------------------------------------------------------------
# 2. Asset Studio (the C++ baker)
# ---------------------------------------------------------------------------

$StudioDir = Join-Path $RepoRoot "ferrum-asset-studio"
$StudioBuild = Join-Path $StudioDir "build"
$Baker = Join-Path $StudioBuild "Release\FerrumAssetStudio.exe"

Write-Step "Building the asset studio"
if (-not (Test-Path (Join-Path $StudioBuild "CMakeCache.txt"))) {
    Invoke-Native -What "cmake configure" -Command { cmake -S $StudioDir -B $StudioBuild }
}
Invoke-Native -What "asset studio build" -Command { cmake --build $StudioBuild --config Release } -Quiet
if (-not (Test-Path $Baker)) { throw "baker not found after build: $Baker" }

# ---------------------------------------------------------------------------
# 3. global.mpak - shared assets: every retail sound, the HUD sprite sheets,
#    and the handful of always-loaded models.
# ---------------------------------------------------------------------------

$ManifestDir = Join-Path $OutDir "_manifests"
if (-not (Test-Path $ManifestDir)) { New-Item -ItemType Directory -Path $ManifestDir | Out-Null }

Write-Step "Collecting the retail sound tree"
$SoundRoot = Join-Path $GameDir "sound"
$soundNames = Get-ChildItem -Path $SoundRoot -Recurse -Filter *.wav -File |
    ForEach-Object { $_.FullName.Substring($SoundRoot.Length + 1).Replace('\', '/') } |
    Sort-Object
$AllSoundsManifest = Join-Path $ManifestDir "all_sounds.txt"
# packSounds() prefixes every line with "sound/", so entries are stored
# exactly as the SDK precaches them (e.g. "player/pl_step1.wav").
Set-Content -Path $AllSoundsManifest -Value $soundNames -Encoding utf8
Write-Host "    $($soundNames.Count) sounds -> $AllSoundsManifest"

$GlobalMpak = Join-Path $OutDir "global.mpak"
Write-Step "Baking global.mpak"
if (-not $Force -and (Test-Path $GlobalMpak)) {
    Write-Host "    already present - pass -Force to rebake"
} else {
    $hudManifest = Join-Path $RepoRoot "hud_sprites_manifest.txt"
    Invoke-Native -What "global.mpak bake" -Quiet -Command {
        & $Baker --game-dir $GameDir --global --output $GlobalMpak --sounds $AllSoundsManifest --sprites $hudManifest
    }
}
Write-Host "    global.mpak ($([math]::Round((Get-Item $GlobalMpak).Length / 1MB, 1)) MB)"

# ---------------------------------------------------------------------------
# 4. Menu archives
# ---------------------------------------------------------------------------

$XashExtras = Join-Path $RepoRoot "xash3d-fwgs\3rdparty\extras\xash-extras"
$ResourceDir = Join-Path $GameDir "resource"
$menuMpak = Join-Path $OutDir "menu.mpak"
$menuBgMpak = Join-Path $OutDir "menu_bg.mpak"

Write-Step "Baking menu.mpak (WON shell art + animated logo)"
if (-not $Force -and (Test-Path $menuMpak)) {
    Write-Host "    already present - pass -Force to rebake"
} elseif (-not (Test-Path $XashExtras)) {
    Write-Warn "xash-extras not found at $XashExtras - the menu will fall back to plain text"
} else {
    $menuManifest = Join-Path $RepoRoot "menu_assets_manifest.txt"
    Invoke-Native -What "menu.mpak bake" -Quiet -Command {
        & $Baker --pack-menu --xash-extras-dir $XashExtras --menu-manifest $menuManifest --resource-dir $ResourceDir --output $menuMpak
    }
}

# --pack-ui resolves resource/BackgroundLayout.txt as <game-dir>/valve/
# resource/, i.e. it wants the Half-Life ROOT, not the valve folder that
# every other mode takes. Derive it rather than asking for both paths.
Write-Step "Baking menu_bg.mpak (resource/background tiles)"
$GameRoot = Split-Path $GameDir -Parent
if (-not $Force -and (Test-Path $menuBgMpak)) {
    Write-Host "    already present - pass -Force to rebake"
} else {
    Invoke-Native -What "menu_bg.mpak bake" -Quiet -Command {
        & $Baker --pack-ui --game-dir $GameRoot --output $menuBgMpak
    }
}

# ---------------------------------------------------------------------------
# 5. Map archives
# ---------------------------------------------------------------------------

Write-Step "Baking map archives"
$bspFiles = Get-ChildItem -Path (Join-Path $GameDir "maps") -Filter *.bsp -File | Sort-Object Name
if ($Maps) {
    $wanted = [System.Collections.Generic.HashSet[string]]::new(
        [string[]]$Maps, [System.StringComparer]::OrdinalIgnoreCase)
    $bspFiles = $bspFiles | Where-Object { $wanted.Contains($_.BaseName) }
    if ($bspFiles.Count -eq 0) { throw "none of the requested maps exist under $GameDir\maps" }
}

$baked = 0
$skipped = 0
$failed = @()
$index = 0
foreach ($bsp in $bspFiles) {
    $index++
    $target = Join-Path $OutDir "$($bsp.BaseName).mpak"
    if (-not $Force -and (Test-Path $target) -and (Get-Item $target).LastWriteTime -gt $bsp.LastWriteTime) {
        $skipped++
        continue
    }
    Write-Host ("    [{0,3}/{1}] {2}" -f $index, $bspFiles.Count, $bsp.Name)
    # Baker chatter is per-face/per-texture and would bury the progress
    # line; only its exit code matters here, and a failure is reported
    # below rather than aborting the remaining ~120 maps.
    try {
        Invoke-Native -What $bsp.BaseName -Quiet -Command {
            & $Baker --game-dir $GameDir --map $bsp.Name --output $target
        }
        if (-not (Test-Path $target)) { throw "$($bsp.BaseName) produced no archive" }
        $baked++
    } catch {
        $failed += $bsp.BaseName
        Write-Warn "$($bsp.BaseName) failed to bake: $($_.Exception.Message)"
    }
}
Write-Host "    baked $baked, skipped $skipped up-to-date, failed $($failed.Count)"

# ---------------------------------------------------------------------------
# 6. Loose runtime files the engine still opens directly from disk
# ---------------------------------------------------------------------------

Write-Step "Copying loose runtime files"

# music.rs reads valve/media/cdaudio.txt and streams valve/media/*.mp3.
$mediaSrc = Join-Path $GameDir "media"
$mediaDst = Join-Path $OutDir "valve\media"
if (Test-Path $mediaSrc) {
    if (-not (Test-Path $mediaDst)) { New-Item -ItemType Directory -Path $mediaDst -Force | Out-Null }
    Copy-Item (Join-Path $mediaSrc "*") $mediaDst -Recurse -Force
    Write-Host "    valve/media ($((Get-ChildItem $mediaDst -File).Count) files)"
} else {
    Write-Warn "no valve/media in the game dir - background music will be silent"
}

# main.rs plays the menu rollover/click straight off disk by path.
$uiSrc = Join-Path $GameDir "sound\UI"
$uiDst = Join-Path $OutDir "valve\sound\UI"
if (Test-Path $uiSrc) {
    if (-not (Test-Path $uiDst)) { New-Item -ItemType Directory -Path $uiDst -Force | Out-Null }
    Copy-Item (Join-Path $uiSrc "*.wav") $uiDst -Force
    Write-Host "    valve/sound/UI ($((Get-ChildItem $uiDst -File).Count) files)"
} else {
    Write-Warn "no valve/sound/UI in the game dir - menu clicks will be silent"
}

# config.rs's exec_file (GameDLLInit's real SERVER_COMMAND("exec
# skill.cfg\n")) reads this straight off disk, same convention as
# valve/media/cdaudio.txt above. Without it every sk_* balance cvar stays
# at its hardcoded 0 default - monster health, weapon damage, all zero.
$skillSrc = Join-Path $GameDir "skill.cfg"
$skillDst = Join-Path $OutDir "valve\skill.cfg"
if (Test-Path $skillSrc) {
    Copy-Item $skillSrc $skillDst -Force
    Write-Host "    valve/skill.cfg"
} else {
    Write-Warn "no skill.cfg in the game dir - every sk_* balance cvar stays at its hardcoded 0 default"
}

# ui.rs enumerates save/*.sav; an absent directory is handled, but the
# folder makes the LoadGame screen's intent obvious to anyone poking around.
$saveDir = Join-Path $OutDir "save"
if (-not (Test-Path $saveDir)) { New-Item -ItemType Directory -Path $saveDir | Out-Null }

# ---------------------------------------------------------------------------
# 7. Launcher
# ---------------------------------------------------------------------------

$bootMap = if (Test-Path (Join-Path $OutDir "c0a0.mpak")) { "c0a0.mpak" } else { (Get-ChildItem $OutDir -Filter *.mpak | Select-Object -First 1).Name }

$runner = @"
# Launches the packaged build. The engine resolves every archive relative to
# its working directory, so this must run from the folder it lives in.
Set-Location `$PSScriptRoot
`$env:FERRUM_MAP = if (`$args.Count -gt 0) { "`$(`$args[0]).mpak" } else { "$bootMap" }
Write-Host "Booting ferrum-engine with FERRUM_MAP=`$env:FERRUM_MAP"
& "`$PSScriptRoot\ferrum-engine.exe"
"@
Set-Content -Path (Join-Path $OutDir "run.ps1") -Value $runner -Encoding utf8

# ---------------------------------------------------------------------------
# 8. Summary
# ---------------------------------------------------------------------------

$mpaks = Get-ChildItem $OutDir -Filter *.mpak -File
$totalMb = [math]::Round(($mpaks | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Step "Done"
Write-Host "    $($mpaks.Count) archives, $totalMb MB total"
Write-Host "    run it with:  ./ferrum56-build-test/run.ps1        (boots $bootMap)"
Write-Host "                  ./ferrum56-build-test/run.ps1 c1a1   (boots a specific map)"
if ($failed.Count -gt 0) {
    Write-Warn "$($failed.Count) map(s) failed to bake: $($failed -join ', ')"
    exit 1
}
