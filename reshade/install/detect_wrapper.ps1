<#
.SYNOPSIS
    Report which D3D8 wrapper base is present and which install script to run.

.DESCRIPTION
    Read-only. Inspects <gamedir>\d3d8.dll (and siblings) to classify the base
    as crosire d3d8to9, dgVoodoo2, a plain Microsoft d3d8.dll, or none. Also
    reports any ASI loader proxy name and the widescreen.cfg effect flags, so
    you can confirm there is no proxy-DLL collision before installing.

.PARAMETER GameDir
    Folder containing psobb.exe. Defaults to the conventional PSOBB.IO location.
#>
[CmdletBinding()]
param([string]$GameDir)

. (Join-Path $PSScriptRoot '_common.ps1')

$gameDir = Resolve-GameDir $GameDir
Write-Host ""
Write-Step "Game directory: $gameDir"

$kind = Get-WrapperKind $gameDir
Write-Step "Base wrapper: $kind"
switch ($kind) {
    'crosire'  { Write-Ok  "crosire d3d8to9  -> run: install_crosire.ps1  (ReShade as d3d9.dll)" }
    'dgvoodoo' { Write-Ok  "dgVoodoo2        -> run: install_dgvoodoo.ps1 (ReShade as dxgi.dll)" }
    'msnative' { Write-Warn2 "plain Microsoft d3d8.dll - no D3D9/D3D11 wrapper. Install crosire or dgVoodoo2 first." }
    'none'     { Write-Err2 "no d3d8.dll found. Install a wrapper first." }
}

$loaders = Get-AsiLoaderInfo $gameDir
if ($loaders.Count -gt 0) {
    Write-Step "ASI loader proxy file(s): $($loaders -join ', ')"
    Write-Info "Avoid using any of these names for the ReShade proxy DLL."
    if ($kind -eq 'crosire' -and ($loaders -contains 'd3d9.dll')) {
        Write-Err2 "COLLISION: crosire needs d3d9.dll for ReShade but the ASI loader already uses d3d9.dll."
    }
    if ($kind -eq 'dgvoodoo' -and ($loaders -contains 'dxgi.dll')) {
        Write-Warn2 "dxgi.dll is taken by the ASI loader; install ReShade as -DllName d3d11.dll instead."
    }
} else {
    Write-Step "No ASI-loader proxy DLL detected among the usual names."
}

$cfg = Read-WidescreenCfg $gameDir
Write-Step "widescreen.cfg: MSAA=$($cfg.MSAA) SMAA=$($cfg.SMAA) SSAO=$($cfg.SSAO) CelShader=$($cfg.CelShader) DOF=$($cfg.DOF) HDR=$($cfg.HDR) HUDScale=$($cfg.HUDScale)"
Write-Host ""
