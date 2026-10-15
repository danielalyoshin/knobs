# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Renders knobs's logo files in assets\ from the brand's masters in brand\.

.DESCRIPTION
brand\ is the knobs-brand repository, a git submodule. From its logo files,
with tools\render-icon.ps1 and tools\render-installer-images.ps1, this writes:
  assets\knobs.ico              the app icon: the colour mark with no tile,
                                the graphite dial and the red sweep
                                (brand\logo\svg\knobs-mark-dark.svg)
  assets\knobs-on-dark.ico      the colour mark for dark grounds, the white
                                dial, for the windows and notifications
                                knobs shows on one (knobs-mark-light.svg)
  assets\knobs-tray-white.ico   the tray icon on a dark taskbar: the white
                                one-color mark, and at 16 and 20 px the knob
                                alone (knobs-knob-white.svg)
  assets\knobs-tray-black.ico   the tray icon on a light taskbar: the black
                                one-color mark, and the knob alone at 16
                                and 20 px (knobs-knob-black.svg)
  assets\installer\*.png        the installer's images, from the dark lockups
                                on the brand's light ground
Each icon frames the mark as the brand's favicon does (its viewBox, in
brand\web\favicon.svg), so the mark fills its square.

Then it writes assets\brand-sources.txt, which says what they were made
from: the SHA-256 of each file used, with its line endings counted as LF so
the hash is the same on any checkout, and the tokens used, with their values.
tools\check-brand-assets.cmake, run as a CTest test, compares that with
brand\ as it is, so the tests fail when brand\ moves on until this is run
again. packaging\package.ps1 gives the installer the ground recorded there,
so the installer's color always matches its images.

.EXAMPLE
tools\render-brand-assets.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$brand = Join-Path $root 'brand'
$assets = Join-Path $root 'assets'
if (-not (Test-Path "$brand\logo\svg")) { throw "brand\ is empty. Run: git submodule update --init" }

$markDark = 'brand\logo\svg\knobs-mark-dark.svg'
$markLight = 'brand\logo\svg\knobs-mark-light.svg'
$markWhite = 'brand\logo\svg\knobs-mark-white.svg'
$markBlack = 'brand\logo\svg\knobs-mark-black.svg'
$knobWhite = 'brand\logo\svg\knobs-knob-white.svg'
$knobBlack = 'brand\logo\svg\knobs-knob-black.svg'
$favicon = 'brand\web\favicon.svg'
$stacked = 'brand\logo\svg\knobs-lockup-stacked-dark.svg'
$horizontal = 'brand\logo\svg\knobs-lockup-horizontal-dark.svg'
$tokens = 'brand\dist\tokens.flat.json'

$groundToken = 'scheme.light.ground'
$ground = (Get-Content -Raw (Join-Path $root $tokens) | ConvertFrom-Json).$groundToken
if ($ground -notmatch '^#[0-9a-f]{6}$') { throw "$tokens has no $groundToken color." }

# The favicon's frame: the mark alone, filling its square.
$frame = [regex]::Match((Get-Content -Raw (Join-Path $root $favicon)), 'viewBox="([^"]*)"').Groups[1].Value
if (-not $frame) { throw "$favicon has no viewBox." }
# The tray's small icon at display scales from 100% to 400%. At 16 and 20 px
# it's the knob alone, as the brand has it, framed so the knob fills it.
$traySizes = @(16, 20, 24, 28, 32, 36, 40, 48, 56, 64)
$knobSizes = @(16, 20)
$knobFrame = '14 14 72 72'

$renderIcon = Join-Path $PSScriptRoot 'render-icon.ps1'
& $renderIcon -Svg (Join-Path $root $markDark) -Out "$assets\knobs.ico" -ViewBox $frame
& $renderIcon -Svg (Join-Path $root $markLight) -Out "$assets\knobs-on-dark.ico" -ViewBox $frame
& $renderIcon -Svg (Join-Path $root $markWhite) -Out "$assets\knobs-tray-white.ico" -Sizes $traySizes -ViewBox $frame `
    -SmallSvg (Join-Path $root $knobWhite) -SmallViewBox $knobFrame -SmallSizes $knobSizes
& $renderIcon -Svg (Join-Path $root $markBlack) -Out "$assets\knobs-tray-black.ico" -Sizes $traySizes -ViewBox $frame `
    -SmallSvg (Join-Path $root $knobBlack) -SmallViewBox $knobFrame -SmallSizes $knobSizes
& (Join-Path $PSScriptRoot 'render-installer-images.ps1') -Ground $ground

# A file's SHA-256, its CRLFs read as LFs, as check-brand-assets.cmake reads it.
function Get-SourceHash([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $root $path))
    $lf = New-Object System.Collections.Generic.List[byte] $bytes.Length
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13 -and $i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10) { continue }
        $lf.Add($bytes[$i])
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($lf.ToArray())) -replace '-', '').ToLowerInvariant()
}

$lines = @(
    '# What assets\ was made from, by tools\render-brand-assets.ps1. Each file''s SHA-256,',
    '# with CRLF read as LF, then each token with its value. Checked by',
    '# tools\check-brand-assets.cmake. Don''t edit: run render-brand-assets.ps1 again.'
)
foreach ($path in @($markDark, $markLight, $markWhite, $markBlack, $knobWhite, $knobBlack, $favicon, $stacked, $horizontal)) {
    $lines += "$(Get-SourceHash $path)  $($path -replace '\\', '/')"
}
$lines += "$groundToken  $ground"
[System.IO.File]::WriteAllText("$assets\brand-sources.txt", ($lines -join "`n") + "`n",
                               (New-Object System.Text.UTF8Encoding $false))
Write-Output "Wrote assets\brand-sources.txt."
