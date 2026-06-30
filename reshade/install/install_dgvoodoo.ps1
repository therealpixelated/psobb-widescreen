<#
.SYNOPSIS
    Install ReShade (Add-on edition) for PSOBB on top of the dgVoodoo2 base wrapper.

.DESCRIPTION
    Chain B:  psobb.exe -> d3d8.dll (dgVoodoo) -> [host D3D11] ; ReShade hooks the
    D3D11 swapchain as dxgi.dll.

        <gamedir>\
          d3d8.dll        <- dgVoodoo MS\d3d8.dll  (LEFT AS-IS; must already exist)
          dxgi.dll        <- ReShade addon DLL (this script installs it)
          dgVoodoo.conf   <- patched: Antialiasing -> off (depth-safe), watermark off
          ReShade.ini     <- authored by this script
          reshade-shaders\Shaders\, \Textures\, PSOBBIO.ini

    WHY dxgi.dll: dgVoodoo's output API is host D3D11, whose presentation goes
    through DXGI. ReShade named dxgi.dll loads into the process and wraps the
    D3D11 device dgVoodoo creates. (Fallback name: d3d11.dll, if dxgi proxy fails.)

    DEPTH CAVEAT: the D3D8 -> dgVoodoo(D3D11) -> ReShade depth path is the
    historically fragile one. SSAO + DOF need depth; SMAA/Cel/HDR do not.
    Run the depth acceptance gate (see README) before trusting SSAO/DOF; if it
    fails, re-run this script with -NoDepth to ship the color-only preset.

    TRANSPARENCY: the ONLY thing downloaded is ReShade itself, from reshade.me,
    pinned to a specific version, and its URL + destination + SHA256 are printed.
    Nothing is downloaded or run without notice. <gamedir>\patches is never touched.

.PARAMETER GameDir
    Folder containing psobb.exe. Defaults to the conventional PSOBB.IO location.

.PARAMETER DllName
    ReShade proxy DLL name. Default 'dxgi.dll'. Use 'd3d11.dll' as a fallback.

.PARAMETER NoDepth
    Force the color-only preset (drops SSAO + DOF). Use when the depth gate fails.

.PARAMETER NoAAEdit
    Do NOT modify dgVoodoo.conf antialiasing (leave the wrapper's AA as-is).

.PARAMETER Force
    Re-download / re-extract ReShade even if cached.

.PARAMETER DryRun
    Print what WOULD happen (detection + plan) without writing anything.

.EXAMPLE
    pwsh -File install_dgvoodoo.ps1
    pwsh -File install_dgvoodoo.ps1 -GameDir 'D:\Games\PSOBB.IO' -NoDepth
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$DllName = 'dxgi.dll',
    [switch]$NoDepth,
    [switch]$NoAAEdit,
    [switch]$Force,
    [switch]$DryRun
)

. (Join-Path $PSScriptRoot '_common.ps1')

Write-Host ""
Write-Host "  PSOBB ReShade installer - dgVoodoo2 base (Chain B)" -ForegroundColor White
Write-Host "  ===================================================" -ForegroundColor White

# --- 1. Resolve + verify base wrapper -------------------------------------
$gameDir = Resolve-GameDir $GameDir
Write-Step "Game directory: $gameDir"

$kind = Get-WrapperKind $gameDir
Write-Step "Detected base wrapper: $kind"
if ($kind -ne 'dgvoodoo') {
    Write-Err2 "This script is for the dgVoodoo2 base, but the detected base is '$kind'."
    if ($kind -eq 'crosire') { Write-Err2 "Use install_crosire.ps1 instead." }
    elseif ($kind -eq 'msnative') { Write-Err2 "A plain Microsoft d3d8.dll is present; no D3D9/D3D11 wrapper to hook. Install dgVoodoo2 or crosire first." }
    elseif ($kind -eq 'none') { Write-Err2 "No d3d8.dll found. Install dgVoodoo2 (its MS\d3d8.dll) into the game folder first." }
    throw "Refusing to install on a mismatched base."
}
Write-Ok "dgVoodoo2 base confirmed (dgVoodoo.conf present)."

# --- 2. ASI loader collision check ----------------------------------------
$loaders = Get-AsiLoaderInfo $gameDir
if ($loaders -contains $DllName.ToLower()) {
    Write-Err2 "An ASI loader appears to occupy '$DllName' already. Choose a different -DllName (e.g. d3d11.dll) to avoid a collision."
    throw "Proxy DLL name collides with the ASI loader."
}
if ($loaders.Count -gt 0) { Write-Info "ASI loader detected as: $($loaders -join ', ') (no collision with $DllName)" }

# --- 3. Read effect flags --------------------------------------------------
$cfg = Read-WidescreenCfg $gameDir
Write-Step "widescreen.cfg effects: MSAA=$($cfg.MSAA) SMAA=$($cfg.SMAA) SSAO=$($cfg.SSAO) CelShader=$($cfg.CelShader) DOF=$($cfg.DOF) HDR=$($cfg.HDR)"

# --- DryRun bail -----------------------------------------------------------
if ($DryRun) {
    Write-Warn2 "DryRun: no changes written. Plan:"
    Write-Info  "  - drop ReShade addon DLL as: $(Join-Path $gameDir $DllName)"
    Write-Info  "  - patch dgVoodoo.conf Antialiasing -> off, watermark -> false  (unless -NoAAEdit)"
    Write-Info  "  - copy bundled shaders -> reshade-shaders\Shaders + \Textures"
    Write-Info  "  - generate preset (NoDepth=$NoDepth) + ReShade.ini"
    return
}

# --- 4. dgVoodoo.conf: AA off (depth-safe) + watermark off -----------------
if (-not $NoAAEdit) {
    $confPath = Join-Path $gameDir 'dgVoodoo.conf'
    $conf = Get-Content -LiteralPath $confPath -Raw
    $bak = "$confPath.reshade-bak"
    if (-not (Test-Path $bak)) { Copy-Item -LiteralPath $confPath -Destination $bak; Write-Info "backed up dgVoodoo.conf" }
    # MSAA breaks ReShade depth resolve. Force Antialiasing = off for both the
    # [DirectX] and [Glide] sections. Watermark off.
    $new = [System.Text.RegularExpressions.Regex]::Replace(
        $conf, '(?m)^\s*Antialiasing\s*=.*$', 'Antialiasing                        = off')
    $new = [System.Text.RegularExpressions.Regex]::Replace(
        $new, '(?m)^\s*dgVoodooWatermark\s*=.*$', 'dgVoodooWatermark                   = false')
    # Anisotropic filtering: force 16x. AF is a texture-SAMPLING setting (the wrapper's
    # job, not a ReShade post-shader) and is depth-safe, so we always enable it. The
    # ^\s*Filtering anchor only hits the [DirectX] Filtering key, not TMUFiltering.
    $new = [System.Text.RegularExpressions.Regex]::Replace(
        $new, '(?m)^\s*Filtering\s*=.*$', 'Filtering                           = 16')
    if ($new -ne $conf) {
        [System.IO.File]::WriteAllText($confPath, ($new -replace "`r?`n","`r`n"))
        Write-Ok "dgVoodoo.conf: Antialiasing=off (depth-safe), Filtering=16 (16x AF), Watermark=false"
    } else {
        Write-Info "dgVoodoo.conf already AA-off / AF-16x / watermark-off"
    }
    if ($cfg.MSAA -eq 1) {
        Write-Warn2 "widescreen.cfg MSAA=1, but wrapper MSAA must be OFF for ReShade depth (SSAO/DOF). AA is provided by SMAA instead."
    }
} else {
    Write-Warn2 "-NoAAEdit: leaving dgVoodoo.conf AA as-is. If AA is ON, ReShade depth (SSAO/DOF) will be disabled."
}

# --- 5. Fetch + place ReShade as the proxy DLL -----------------------------
$reshadeDll = Get-ReShadeDll -Force:$Force
$dst = Join-Path $gameDir $DllName
Copy-Managed $reshadeDll $dst
Write-Ok "ReShade installed as $DllName"

# --- 6. Shaders + preset + ini ---------------------------------------------
Install-BundledShaders $gameDir
if ($cfg.SMAA -eq 1) { Install-SMAA $gameDir }
$presetName = Build-Preset $gameDir $cfg ([bool]$NoDepth)
Build-ReShadeIni $gameDir $presetName

# --- 7. Done ---------------------------------------------------------------
Write-Host ""
Write-Ok "Install complete (dgVoodoo2 / Chain B)."
Write-Host ""
Write-Host "  NEXT STEPS:" -ForegroundColor White
Write-Host "   1. Launch PSOBB. Press Home to open the ReShade overlay." -ForegroundColor Gray
Write-Host "   2. Confirm the renderer reads 'Direct3D 11' (dgVoodoo's host API)." -ForegroundColor Gray
Write-Host "   3. DEPTH GATE: enable DisplayDepth.fx; a correct near-dark/far-light" -ForegroundColor Gray
Write-Host "      gradient = depth works -> SSAO/DOF are valid. Flat/garbage = depth" -ForegroundColor Gray
Write-Host "      broken -> re-run this script with -NoDepth." -ForegroundColor Gray
Write-Host "   4. Uninstall: delete <gamedir>\$DllName and \reshade-shaders, restore" -ForegroundColor Gray
Write-Host "      any *.reshade-bak files. (See README.)" -ForegroundColor Gray
Write-Host ""
