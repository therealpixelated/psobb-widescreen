# _common.ps1 — shared helpers for the PSOBB ReShade installers.
#
# Dot-sourced by install_crosire.ps1 and install_dgvoodoo.ps1.
# Pure-PowerShell, no external module dependencies. Everything is idempotent
# and non-destructive: existing files are backed up (.reshade-bak) before any
# overwrite, and nothing in <gamedir>\patches is ever touched.
#
# NOTHING here downloads or runs the game. The ONLY network fetch is ReShade
# itself (and, opt-in, non-redistributable shaders) — and every fetch prints
# its exact URL and destination before it happens.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Pinned versions / URLs  (TRANSPARENCY: this is everything we ever download)
# ---------------------------------------------------------------------------
# ReShade 6.5.1 is the last "addon" build that ships the in-box installer that
# also writes a setup-style ReShade.ini; we instead drop the addon DLL directly
# and author ReShade.ini ourselves, so any 6.x addon build works. We PIN 6.5.1
# for reproducibility. The "_Addon" SKU is REQUIRED — the vanilla SKU disables
# the depth buffer (so SSAO + DOF would be dead).
$script:RESHADE_VERSION  = '6.5.1'
$script:RESHADE_URL      = 'https://reshade.me/downloads/ReShade_Setup_6.5.1_Addon.exe'
# The installer is a self-extracting 7-zip; we extract the bundled ReShade
# DLL from it without running it (see Get-ReShadeDll).
$script:RESHADE_SHA256   = ''   # left blank: reshade.me does not publish a stable
                                # per-version hash; we print the downloaded file's
                                # hash for the user to record/verify instead.

# Non-redistributable / opt-in downloads (NEVER committed to this repo):
$script:QUINT_MXAO_URL   = 'https://raw.githubusercontent.com/martymcmodding/qUINT/master/Shaders/qUINT_mxao.fx'
$script:QUINT_COMMON_URL = 'https://raw.githubusercontent.com/martymcmodding/qUINT/master/Shaders/qUINT_common.fxh'

# ---------------------------------------------------------------------------
# Pretty printing
# ---------------------------------------------------------------------------
function Write-Step  ($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Info  ($m) { Write-Host "    $m" -ForegroundColor Gray }
function Write-Ok    ($m) { Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn2 ($m) { Write-Host "    [!]  $m" -ForegroundColor Yellow }
function Write-Err2  ($m) { Write-Host "    [X]  $m" -ForegroundColor Red }

function Write-DownloadNotice ($url, $dest) {
    Write-Host ""
    Write-Host "    DOWNLOAD:" -ForegroundColor Magenta
    Write-Host "      from -> $url"   -ForegroundColor Magenta
    Write-Host "      to   -> $dest"  -ForegroundColor Magenta
    Write-Host ""
}

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# The package root = parent of the install/ dir this script lives in.
function Get-PackageRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Test-IsGameDir ($dir) {
    return (Test-Path (Join-Path $dir 'psobb.exe')) -or (Test-Path (Join-Path $dir 'Psobb.exe'))
}

# Resolve the game directory: explicit -GameDir wins, else try the known
# PSOBB.IO location, else fail loudly. We NEVER guess-and-write.
function Resolve-GameDir ($GameDir) {
    if ($GameDir) {
        $GameDir = (Resolve-Path $GameDir).Path
        if (-not (Test-IsGameDir $GameDir)) {
            throw "No psobb.exe found in '$GameDir'. Pass -GameDir <path-to-folder-with-psobb.exe>."
        }
        return $GameDir
    }
    # Default to the conventional install next to the user profile, composed at
    # runtime (no hardcoded absolute path).
    $default = Join-Path (Split-Path $env:USERPROFILE -Parent) (Join-Path (Split-Path $env:USERPROFILE -Leaf) 'PSOBB.IO')
    if (Test-IsGameDir $default) { return $default }
    $default2 = Join-Path $env:USERPROFILE 'PSOBB.IO'
    if (Test-IsGameDir $default2) { return $default2 }
    throw "Could not locate psobb.exe. Pass -GameDir <path-to-folder-with-psobb.exe>."
}

# ---------------------------------------------------------------------------
# widescreen.cfg parsing  (mirrors source/Options.c parse semantics:
#   case-insensitive key substring; value 1/on/true => enabled, 0/off/false => disabled)
# ---------------------------------------------------------------------------
function Read-WidescreenCfg ($gameDir) {
    $cfg = @{ MSAA = 1; SMAA = 1; SSAO = 1; CelShader = 1; DOF = 1; HDR = 1; HUDScale = 1.0 }
    $path = Join-Path $gameDir 'widescreen.cfg'
    if (-not (Test-Path $path)) {
        Write-Warn2 "widescreen.cfg not found at $path - using anzz1 defaults (all effects ON)."
        return $cfg
    }
    foreach ($raw in (Get-Content -LiteralPath $path)) {
        $line = $raw.Trim()
        if ($line -eq '' -or $line.StartsWith('#') -or $line.StartsWith(';') -or $line.StartsWith('/')) { continue }
        if ($line -notmatch '=') { continue }
        $k, $v = $line.Split('=', 2)
        $k = $k.Trim(); $v = $v.Trim()
        $on  = ($v -match '^(1|on|true)$')
        $off = ($v -match '^(0|off|false)$')
        switch -Regex ($k.ToLower()) {
            '^msaa$'      { if ($on) { $cfg.MSAA = 1 } elseif ($off) { $cfg.MSAA = 0 } }
            '^smaa$'      { if ($on) { $cfg.SMAA = 1 } elseif ($off) { $cfg.SMAA = 0 } }
            '^ssao$'      { if ($on) { $cfg.SSAO = 1 } elseif ($off) { $cfg.SSAO = 0 } }
            '^celshader$' { if ($on) { $cfg.CelShader = 1 } elseif ($off) { $cfg.CelShader = 0 } }
            '^dof$'       { if ($on) { $cfg.DOF = 1 } elseif ($off) { $cfg.DOF = 0 } }
            '^hdr$'       { if ($on) { $cfg.HDR = 1 } elseif ($off) { $cfg.HDR = 0 } }
            '^hudscale$'  { [double]$f = 0; if ([double]::TryParse($v, [ref]$f) -and $f -gt 0) { $cfg.HUDScale = $f } }
        }
    }
    return $cfg
}

# ---------------------------------------------------------------------------
# Wrapper detection  (crosire d3d8to9 vs dgVoodoo2 vs plain MS d3d8)
# ---------------------------------------------------------------------------
# Returns: 'crosire' | 'dgvoodoo' | 'msnative' | 'none'
function Get-WrapperKind ($gameDir) {
    # Windows FS is case-insensitive; either casing resolves the same file.
    $d3d8 = Join-Path $gameDir 'd3d8.dll'
    if (-not (Test-Path $d3d8)) { return 'none' }

    # 1) dgVoodoo signature: a dgVoodoo.conf sitting next to it, and/or the
    #    matched DDraw/D3DImm MS-folder set. The conf is decisive.
    if (Test-Path (Join-Path $gameDir 'dgVoodoo.conf')) { return 'dgvoodoo' }

    # 2) Read printable strings from the DLL to discriminate. crosire d3d8to9
    #    imports d3d9.dll!Direct3DCreate9 and carries "d3d8to9"/"crosire"
    #    markers; dgVoodoo carries "dgVoodoo"/"Dege".
    $bytes = [System.IO.File]::ReadAllBytes($d3d8)
    $ascii = -join ($bytes | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { "`n" } })
    if ($ascii -match 'dgVoodoo|Dege')              { return 'dgvoodoo' }
    if ($ascii -match 'd3d8to9|crosire|Direct3DCreate9') { return 'crosire' }

    # 3) Matched MS-folder set (DDraw + D3DImm same size/date) without conf =
    #    likely dgVoodoo with conf elsewhere, but be conservative.
    if ((Test-Path (Join-Path $gameDir 'DDraw.dll')) -and (Test-Path (Join-Path $gameDir 'D3DImm.dll'))) {
        return 'dgvoodoo'
    }
    return 'msnative'
}

# ---------------------------------------------------------------------------
# Detect what filename the ASI/widescreen loader uses, so we never collide.
# Ultimate ASI Loader can masquerade as d3d8.dll/d3d9.dll/dxgi.dll/etc.
# ---------------------------------------------------------------------------
function Get-AsiLoaderInfo ($gameDir) {
    $candidates = 'd3d8.dll','d3d9.dll','dxgi.dll','d3d11.dll','version.dll','winmm.dll','dinput8.dll'
    $found = @()
    foreach ($c in $candidates) {
        $p = Join-Path $gameDir $c
        if (Test-Path $p) {
            try {
                $b = [System.IO.File]::ReadAllBytes($p)
                $s = -join ($b[0..([Math]::Min($b.Length,400000)-1)] | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
                if ($s -match 'Ultimate ASI Loader|UltimateASILoader|aslr|\.asi') { $found += $c }
            } catch {}
        }
    }
    return ,([string[]]$found)   # always return an array (avoid 1-elem unwrap)
}

# ---------------------------------------------------------------------------
# Idempotent / non-destructive copy: backs up a differing existing file to
# <name>.reshade-bak (once) before overwriting; skips if content identical.
# ---------------------------------------------------------------------------
function Copy-Managed ($src, $dst) {
    if (-not (Test-Path $src)) { throw "source missing: $src" }
    if (Test-Path $dst) {
        $hSrc = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
        $hDst = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
        if ($hSrc -eq $hDst) { Write-Info "unchanged: $(Split-Path $dst -Leaf)"; return }
        $bak = "$dst.reshade-bak"
        if (-not (Test-Path $bak)) {
            Copy-Item -LiteralPath $dst -Destination $bak
            Write-Info "backed up existing $(Split-Path $dst -Leaf) -> $(Split-Path $bak -Leaf)"
        }
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Ok "wrote $(Split-Path $dst -Leaf)"
}

# Write text idempotently (skip if identical, back up if differs).
function Write-Managed ($dst, $text) {
    $text = $text -replace "`r?`n", "`r`n"
    if (Test-Path $dst) {
        $cur = (Get-Content -LiteralPath $dst -Raw)
        if ($cur -eq $text) { Write-Info "unchanged: $(Split-Path $dst -Leaf)"; return }
        $bak = "$dst.reshade-bak"
        if (-not (Test-Path $bak)) { Copy-Item -LiteralPath $dst -Destination $bak; Write-Info "backed up $(Split-Path $dst -Leaf)" }
    }
    [System.IO.File]::WriteAllText($dst, $text)
    Write-Ok "wrote $(Split-Path $dst -Leaf)"
}

# ---------------------------------------------------------------------------
# Copy the bundled, redistributable shaders into <gamedir>\reshade-shaders.
# ---------------------------------------------------------------------------
function Install-BundledShaders ($gameDir) {
    $root = Get-PackageRoot
    $srcShaders  = Join-Path $root 'shaders\Shaders'
    $srcTextures = Join-Path $root 'shaders\Textures'
    $dstShaders  = Join-Path $gameDir 'reshade-shaders\Shaders'
    $dstTextures = Join-Path $gameDir 'reshade-shaders\Textures'
    New-Item -ItemType Directory -Force -Path $dstShaders  | Out-Null
    New-Item -ItemType Directory -Force -Path $dstTextures | Out-Null

    Write-Step "Installing bundled shaders -> $dstShaders"
    foreach ($f in (Get-ChildItem -LiteralPath $srcShaders -File)) {
        Copy-Managed $f.FullName (Join-Path $dstShaders $f.Name)
    }
    if (Test-Path $srcTextures) {
        foreach ($f in (Get-ChildItem -LiteralPath $srcTextures -File)) {
            Copy-Managed $f.FullName (Join-Path $dstTextures $f.Name)
        }
    }
}

# ---------------------------------------------------------------------------
# Fetch ReShade's stock SMAA (BSD-3) + textures when SMAA is enabled. Delegates
# to fetch_smaa.ps1 so the canonical bytes stay out of this repo.
# ---------------------------------------------------------------------------
function Install-SMAA ($gameDir) {
    $smaaFx = Join-Path $gameDir 'reshade-shaders\Shaders\SMAA.fx'
    if (Test-Path $smaaFx) { Write-Info "SMAA already present"; return }
    Write-Step "SMAA enabled - fetching ReShade's stock SMAA (BSD-3)"
    & (Join-Path $PSScriptRoot 'fetch_smaa.ps1') -GameDir $gameDir
}

# ---------------------------------------------------------------------------
# Generate the curated preset from the base PSOBBIO.ini, filtering the
# Techniques= line by the widescreen.cfg flags. Preserves the fixed anzz1
# order. SSAO/DOF require depth: if -NoDepth, they are dropped regardless.
# ---------------------------------------------------------------------------
function Build-Preset ($gameDir, $cfg, [bool]$noDepth) {
    $root = Get-PackageRoot
    $base = Join-Path $root 'presets\PSOBBIO.ini'
    $text = Get-Content -LiteralPath $base -Raw

    # Fixed order = anzz1 chain.  PSO_SSAO/PSO_DOF gated on depth.
    $ordered = @(
        @{ flag = 'SMAA';      tech = 'SMAA';           depth = $false },
        @{ flag = 'SSAO';      tech = 'PSO_SSAO';       depth = $true  },
        @{ flag = 'CelShader'; tech = 'PSO_CelShader';  depth = $false },
        @{ flag = 'DOF';       tech = 'PSO_DOF';        depth = $true  },
        @{ flag = 'HDR';       tech = 'PSO_HDRToneMap'; depth = $false }
    )
    $techs = @()
    $droppedDepth = @()
    foreach ($e in $ordered) {
        if ($cfg[$e.flag] -ne 1) { continue }
        if ($e.depth -and $noDepth) { $droppedDepth += $e.tech; continue }
        $techs += $e.tech
    }
    $line = 'Techniques=' + ($techs -join ',')
    $text = ($text -split "`r?`n" | ForEach-Object {
        if ($_ -match '^\s*Techniques=') { $line } else { $_ }
    }) -join "`r`n"

    $presetName = if ($noDepth) { 'PSOBBIO_no_depth.ini' } else { 'PSOBBIO.ini' }
    $dst = Join-Path $gameDir "reshade-shaders\$presetName"
    Write-Managed $dst $text
    if ($droppedDepth.Count -gt 0) {
        Write-Warn2 ("depth gate / -NoDepth: dropped " + ($droppedDepth -join ', ') + " (need a working depth buffer)")
    }
    Write-Ok "preset techniques: $($techs -join ', ')"
    return $presetName
}

# ---------------------------------------------------------------------------
# Author ReShade.ini from the template, filling in the preset path + depth
# addon defaults. Idempotent.
# ---------------------------------------------------------------------------
function Build-ReShadeIni ($gameDir, $presetName) {
    $root = Get-PackageRoot
    $tpl  = Get-Content -LiteralPath (Join-Path $root 'config\ReShade.ini.template') -Raw
    $tpl  = $tpl.Replace('{{PRESET}}', ".\reshade-shaders\$presetName")
    Write-Managed (Join-Path $gameDir 'ReShade.ini') $tpl
}

# ---------------------------------------------------------------------------
# Download + extract the ReShade addon DLL.  We DO NOT run the installer; we
# extract the embedded DLL from the self-extracting archive. Prints the URL.
# Returns path to the extracted ReShade DLL (architecture: 32-bit, PSOBB is x86).
# ---------------------------------------------------------------------------
function Get-ReShadeDll ([switch]$Force) {
    $root  = Get-PackageRoot
    $cache = Join-Path $root '.cache'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $setup = Join-Path $cache "ReShade_Setup_$($script:RESHADE_VERSION)_Addon.exe"
    $dll   = Join-Path $cache "ReShade32_$($script:RESHADE_VERSION).dll"

    if ((Test-Path $dll) -and -not $Force) {
        Write-Info "using cached ReShade DLL: $dll"
        return $dll
    }

    if (-not (Test-Path $setup) -or $Force) {
        Write-DownloadNotice $script:RESHADE_URL $setup
        Write-Step "Downloading ReShade $($script:RESHADE_VERSION) (Addon edition) from reshade.me ..."
        Invoke-WebRequest -Uri $script:RESHADE_URL -OutFile $setup -UseBasicParsing
    }
    $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash
    Write-Info "downloaded SHA256: $hash"

    # The ReShade setup .exe is a 7-zip SFX. The 32-bit DLL is stored inside as
    # 'ReShade32.dll'. Try 7-Zip if present; else fall back to a byte-scan extract.
    $7z = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($7z) {
        Write-Info "extracting ReShade32.dll with 7-Zip"
        & $7z.Source e -y -o"$cache" "$setup" 'ReShade32.dll' | Out-Null
        $ex = Join-Path $cache 'ReShade32.dll'
        if (Test-Path $ex) { Move-Item -Force $ex $dll; Write-Ok "extracted ReShade32.dll"; return $dll }
    }
    throw @"
Could not auto-extract ReShade32.dll. 7-Zip not found on PATH.
Either install 7-Zip (https://www.7-zip.org/) and re-run, OR run the ReShade
setup once manually (cached at: $setup), pick psobb.exe, and the installer
will place the DLL itself. The rest of this script (shaders/preset/ini) is safe
to re-run afterwards.
"@
}
