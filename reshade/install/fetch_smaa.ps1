<#
.SYNOPSIS
    Fetch ReShade's stock SMAA shader (BSD-3) + its area/search textures.

.DESCRIPTION
    SMAA in ReShade is the same Iryoku MIT-core implementation anzz1 used, packaged
    by crosire under BSD-3-clause. It is large (~1000 lines across SMAA.fx + SMAA.fxh)
    and depends on two binary lookup textures (AreaTex.dds / SearchTex.dds). Rather
    than hand-transcribe that into this repo (risking a stale / non-compiling copy),
    we fetch it verbatim from a PINNED crosire/reshade-shaders commit. It is fully
    redistributable; we just keep the canonical bytes out of our tree.

    Called automatically by install_crosire.ps1 / install_dgvoodoo.ps1 when SMAA is
    enabled, or run standalone.

    TRANSPARENCY: prints every source URL + destination before downloading.

.PARAMETER GameDir
    Folder containing psobb.exe. Defaults to the conventional PSOBB.IO location.
#>
[CmdletBinding()]
param([string]$GameDir)

. (Join-Path $PSScriptRoot '_common.ps1')

# Pinned commit of crosire/reshade-shaders (latest branch) that carries SMAA.
$ref  = '4631a0bbb0f875d725416b843a399339e77f2259'
$base = "https://raw.githubusercontent.com/crosire/reshade-shaders/$ref"
$files = @(
    @{ url = "$base/Shaders/SMAA.fx";          rel = 'Shaders\SMAA.fx' },
    @{ url = "$base/Shaders/SMAA.fxh";         rel = 'Shaders\SMAA.fxh' },
    @{ url = "$base/Textures/AreaTex.dds";     rel = 'Textures\AreaTex.dds' },
    @{ url = "$base/Textures/SearchTex.dds";   rel = 'Textures\SearchTex.dds' }
)

$gameDir = Resolve-GameDir $GameDir
$shaderRoot = Join-Path $gameDir 'reshade-shaders'
New-Item -ItemType Directory -Force -Path (Join-Path $shaderRoot 'Shaders')  | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $shaderRoot 'Textures') | Out-Null

Write-Step "Fetching ReShade SMAA (BSD-3) from crosire/reshade-shaders @ $($ref.Substring(0,10))"
foreach ($f in $files) {
    $dst = Join-Path $shaderRoot $f.rel
    if (Test-Path $dst) { Write-Info "exists: $($f.rel) (skipping; delete to refresh)"; continue }
    Write-DownloadNotice $f.url $dst
    Invoke-WebRequest -Uri $f.url -OutFile $dst -UseBasicParsing
    Write-Ok "fetched $($f.rel)"
}
Write-Info "SMAA uses its own AreaTex.dds/SearchTex.dds; our preset sets SMAA to luma edge detection, High preset."
