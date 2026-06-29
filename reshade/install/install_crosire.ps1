<#
.SYNOPSIS
    Install ReShade (Add-on edition) for PSOBB on top of the crosire d3d8to9 base.

.DESCRIPTION
    Chain A:  psobb.exe -> d3d8.dll (crosire d3d8to9) -> d3d9.dll (ReShade) -> system d3d9.dll

        <gamedir>\
          d3d8.dll        <- crosire d3d8to9  (LEFT AS-IS; must already exist)
          d3d9.dll        <- ReShade addon DLL (this script installs it)
          d3dx9_43.dll    <- REQUIRED by crosire; this script checks it is resolvable
          ReShade.ini     <- authored by this script
          reshade-shaders\Shaders\, \Textures\, PSOBBIO.ini

    WHY d3d9.dll: crosire's d3d8.dll imports d3d9.dll!Direct3DCreate9. With ReShade
    sitting next to psobb.exe as d3d9.dll, Windows' app-directory-first DLL search
    resolves that import to ReShade, which then forwards to the system d3d9 and
    wraps the device. No name collision (d3d8 vs d3d9).

    DEPTH: this is the better-tested depth path (native D3D9 generic-depth). SSAO +
    DOF depend on it; SMAA/Cel/HDR are color-only and work regardless.

    MSAA: crosire does not force MSAA, so AA comes from SMAA only. Do NOT enable
    D3D9 MSAA (it would disable ReShade depth -> kill SSAO/DOF).

    TRANSPARENCY: the ONLY thing downloaded is ReShade itself, from reshade.me,
    pinned to a specific version, with its URL + destination + SHA256 printed.
    Nothing is downloaded or run without notice. <gamedir>\patches is never touched.

.PARAMETER GameDir
    Folder containing psobb.exe. Defaults to the conventional PSOBB.IO location.

.PARAMETER DllName
    ReShade proxy DLL name. Default 'd3d9.dll'. (Almost never changed for crosire.)

.PARAMETER NoDepth
    Force the color-only preset (drops SSAO + DOF).

.PARAMETER Force
    Re-download / re-extract ReShade even if cached.

.PARAMETER DryRun
    Print the detection + plan without writing anything.

.EXAMPLE
    pwsh -File install_crosire.ps1
    pwsh -File install_crosire.ps1 -GameDir 'D:\Games\PSOBB' -NoDepth
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$DllName = 'd3d9.dll',
    [switch]$NoDepth,
    [switch]$Force,
    [switch]$DryRun
)

. (Join-Path $PSScriptRoot '_common.ps1')

Write-Host ""
Write-Host "  PSOBB ReShade installer - crosire d3d8to9 base (Chain A)" -ForegroundColor White
Write-Host "  =======================================================" -ForegroundColor White

# --- 1. Resolve + verify base wrapper -------------------------------------
$gameDir = Resolve-GameDir $GameDir
Write-Step "Game directory: $gameDir"

$kind = Get-WrapperKind $gameDir
Write-Step "Detected base wrapper: $kind"
if ($kind -ne 'crosire') {
    Write-Err2 "This script is for the crosire d3d8to9 base, but the detected base is '$kind'."
    if ($kind -eq 'dgvoodoo') { Write-Err2 "Use install_dgvoodoo.ps1 instead (your d3d8.dll is dgVoodoo2)." }
    elseif ($kind -eq 'msnative') { Write-Err2 "A plain Microsoft d3d8.dll is present. Replace it with crosire d3d8to9 first." }
    elseif ($kind -eq 'none') { Write-Err2 "No d3d8.dll found. Install crosire d3d8to9 (its d3d8.dll) into the game folder first." }
    throw "Refusing to install on a mismatched base."
}
Write-Ok "crosire d3d8to9 base confirmed."

# --- 2. d3dx9_43.dll dependency (crosire hard-depends on it) ---------------
$hasLocal = Test-Path (Join-Path $gameDir 'd3dx9_43.dll')
$hasSys   = (Test-Path (Join-Path $env:WINDIR 'SysWOW64\d3dx9_43.dll')) -or (Test-Path (Join-Path $env:WINDIR 'System32\d3dx9_43.dll'))
if (-not ($hasLocal -or $hasSys)) {
    Write-Warn2 "d3dx9_43.dll not found locally or in the system. crosire d3d8to9 needs it (DirectX End-User Runtime), or PSOBB will fail to start."
    Write-Warn2 "Install the DirectX End-User Runtime, or drop a 32-bit d3dx9_43.dll next to psobb.exe."
} else {
    Write-Ok "d3dx9_43.dll resolvable ($([string]::Join('+', @(if($hasLocal){'local'}; if($hasSys){'system'}))))."
}

# --- 3. ASI loader collision check ----------------------------------------
$loaders = Get-AsiLoaderInfo $gameDir
if ($loaders -contains $DllName.ToLower()) {
    Write-Err2 "An ASI loader appears to occupy '$DllName'. With crosire this is a hard conflict (both want d3d9.dll). Resolve before installing."
    throw "Proxy DLL name collides with the ASI loader."
}
if ($loaders.Count -gt 0) { Write-Info "ASI loader detected as: $($loaders -join ', ') (no collision with $DllName)" }

# --- 4. Read effect flags --------------------------------------------------
$cfg = Read-WidescreenCfg $gameDir
Write-Step "widescreen.cfg effects: MSAA=$($cfg.MSAA) SMAA=$($cfg.SMAA) SSAO=$($cfg.SSAO) CelShader=$($cfg.CelShader) DOF=$($cfg.DOF) HDR=$($cfg.HDR)"
if ($cfg.MSAA -eq 1) {
    Write-Warn2 "widescreen.cfg MSAA=1: crosire does not force D3D9 MSAA, and you should NOT enable it (it kills ReShade depth). AA is provided by SMAA."
}

# --- DryRun bail -----------------------------------------------------------
if ($DryRun) {
    Write-Warn2 "DryRun: no changes written. Plan:"
    Write-Info  "  - drop ReShade addon DLL as: $(Join-Path $gameDir $DllName)"
    Write-Info  "  - copy bundled shaders -> reshade-shaders\Shaders + \Textures"
    Write-Info  "  - generate preset (NoDepth=$NoDepth) + ReShade.ini"
    return
}

# --- 5. Fetch + place ReShade as d3d9.dll ----------------------------------
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
Write-Ok "Install complete (crosire / Chain A)."
Write-Host ""
Write-Host "  NEXT STEPS:" -ForegroundColor White
Write-Host "   1. Launch PSOBB. Press Home to open the ReShade overlay." -ForegroundColor Gray
Write-Host "   2. Confirm the renderer reads 'Direct3D 9'." -ForegroundColor Gray
Write-Host "   3. DEPTH GATE: enable DisplayDepth.fx; a correct near-dark/far-light" -ForegroundColor Gray
Write-Host "      gradient = depth works -> SSAO/DOF are valid. Flat/garbage = depth" -ForegroundColor Gray
Write-Host "      broken -> re-run this script with -NoDepth." -ForegroundColor Gray
Write-Host "   4. Uninstall: delete <gamedir>\$DllName and \reshade-shaders, restore" -ForegroundColor Gray
Write-Host "      any *.reshade-bak files. (See README.)" -ForegroundColor Gray
Write-Host ""
