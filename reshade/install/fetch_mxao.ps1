<#
.SYNOPSIS
    OPT-IN download of qUINT MXAO as a higher-quality SSAO alternative.

.DESCRIPTION
    MXAO (martymcmodding/qUINT) is NOT redistributable ("All rights reserved";
    a PSOBB preset repo was DMCA'd in 2025 for bundling it). This repo therefore
    NEVER commits it. This script downloads it ON DEMAND, with explicit consent,
    straight into your local reshade-shaders\Shaders folder so it stays out of
    version control.

    After fetching, enable 'MXAO' in the ReShade overlay manually, or add it to
    your preset's Techniques= line in place of PSO_SSAO.

    TRANSPARENCY: prints the exact source URLs and destination before downloading.

.PARAMETER GameDir
    Folder containing psobb.exe. Defaults to the conventional PSOBB.IO location.

.PARAMETER Yes
    Skip the interactive consent prompt (for unattended use).
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [switch]$Yes
)

. (Join-Path $PSScriptRoot '_common.ps1')

$gameDir = Resolve-GameDir $GameDir
$dstDir  = Join-Path $gameDir 'reshade-shaders\Shaders'
if (-not (Test-Path $dstDir)) {
    throw "reshade-shaders\Shaders not found. Run install_crosire.ps1 / install_dgvoodoo.ps1 first."
}

Write-Host ""
Write-Warn2 "qUINT MXAO is NOT redistributable and is licensed 'All rights reserved'."
Write-Warn2 "This script downloads it directly from the author's GitHub for your personal use only."
Write-Warn2 "It is never committed to this repository."
Write-DownloadNotice $script:QUINT_MXAO_URL   (Join-Path $dstDir 'qUINT_mxao.fx')
Write-DownloadNotice $script:QUINT_COMMON_URL (Join-Path $dstDir 'qUINT_common.fxh')

if (-not $Yes) {
    $ans = Read-Host "Download qUINT MXAO now? (y/N)"
    if ($ans -notmatch '^(y|yes)$') { Write-Info "aborted."; return }
}

Invoke-WebRequest -Uri $script:QUINT_MXAO_URL   -OutFile (Join-Path $dstDir 'qUINT_mxao.fx')   -UseBasicParsing
Invoke-WebRequest -Uri $script:QUINT_COMMON_URL -OutFile (Join-Path $dstDir 'qUINT_common.fxh') -UseBasicParsing
Write-Ok "MXAO downloaded to $dstDir"
Write-Info "Enable 'MXAO' in the ReShade overlay, or swap it for PSO_SSAO in your preset Techniques= line."
Write-Info "MXAO runs best BEFORE color effects; place it first if you add it to the preset."
