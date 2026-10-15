# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Packages a Release build of knobs as an installer and a portable zip.

.DESCRIPTION
Takes knobs.exe from build\x64\Release (build it first: cmake --build --preset
release), or the exe given, and writes to build\package:
  <name>-<version>-setup.exe      the installer, built with Inno Setup 6
  <name>-<version>-portable.zip   the portable copy: a folder with knobs.exe and
                                  portable_mode.txt, which keeps its data beside it
  SHA256SUMS.txt                  the checksums of both
The name comes from src\app_info.h and the version from CMakeLists.txt. The
exe's own version has to match.

The installer carries VB-Audio's VB-CABLE driver pack, as VB-Audio ships it, to
offer it when there's no virtual cable. It's downloaded from vb-audio.com into
build\vb-cable the first time, and its SHA-256 checked against the one below,
so a pack VB-Audio changed fails the build until it's checked and the hash
updated. VB-Audio allows it to be bundled, with credit
(https://vb-audio.com/Services/licensing.htm): NOTICE.txt and the installer
give it.

.PARAMETER Exe
The knobs.exe to package. By default, build\x64\Release's.

.PARAMETER Sign
Signs knobs.exe before it's packaged, and the installer and its uninstaller,
with SignScript.

.PARAMETER SignScript
What signs: a script that takes -Description, -Url and the files to sign. By
default, sign.ps1, which uses Azure Artifact Signing and says what it needs.

.PARAMETER Iscc
Inno Setup's compiler. By default, ISCC.exe on PATH or in Inno Setup 6's usual
folders.

.PARAMETER NoInstaller
Writes only the portable zip.

.PARAMETER VbCablePack
VB-Audio's driver pack, to use instead of downloading it. Its hash is checked
all the same.
#>
param(
  [string]$Exe,
  [switch]$Sign,
  [string]$SignScript = "$PSScriptRoot\sign.ps1",
  [string]$Iscc,
  [switch]$NoInstaller,
  [string]$VbCablePack
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$name = [regex]::Match((Get-Content -Raw "$root\src\app_info.h"), '#define KNOBS_DISPLAY_NAME "([^"]+)"').Groups[1].Value
$version = [regex]::Match((Get-Content -Raw "$root\CMakeLists.txt"), 'project\(\w+ VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $name -or -not $version) { throw "Couldn't read the name from src\app_info.h or the version from CMakeLists.txt." }

$url = 'https://github.com/danielalyoshin/knobs'
if (-not $Exe) { $Exe = "$root\build\x64\Release\$name.exe" }
if (-not (Test-Path $Exe)) { throw "$Exe doesn't exist. Build it first: cmake --build --preset release" }
$exeVersion = (Get-Item $Exe).VersionInfo.ProductVersion
if ($exeVersion -ne $version) { throw "$Exe is version $exeVersion, but CMakeLists.txt says $version. Build again." }
if ($Sign -and -not (Test-Path $SignScript)) { throw "$SignScript doesn't exist." }

$out = "$root\build\package"
$stage = "$out\stage\$name"
Remove-Item -Recurse -Force "$out" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# The text files, with the name and version filled in, and Windows line endings.
function Write-Text([string]$from, [string]$to) {
  $text = (Get-Content -Raw $from) -replace '@NAME@', $name -replace '@VERSION@', $version
  [IO.File]::WriteAllText($to, ($text -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}
Copy-Item $Exe "$stage\$name.exe"
# Signed before it's packaged, so the zip and the installer carry the signed exe.
if ($Sign) { & $SignScript -Description $name -Url $url "$stage\$name.exe" }
Write-Text "$root\LICENSE" "$stage\LICENSE.txt"
Write-Text "$PSScriptRoot\NOTICE.txt" "$stage\NOTICE.txt"

# The portable zip: the same files, the marker, and one folder to unzip.
$portable = "$out\portable\$name"
New-Item -ItemType Directory -Force $portable | Out-Null
Copy-Item "$stage\*" $portable
Write-Text "$PSScriptRoot\portable_mode.txt" "$portable\portable_mode.txt"
$zip = "$out\$name-$version-portable.zip"
# Each entry named by hand: Windows PowerShell's Compress-Archive and .NET
# Framework's ZipFile write backslashes, where the zip format has forward
# slashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($file in Get-ChildItem -File $portable) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "$name/$($file.Name)",
      [IO.Compression.CompressionLevel]::Optimal)
  }
} finally {
  $archive.Dispose()
}
$artifacts = @($zip)

if (-not $NoInstaller) {
  # VB-Audio's driver pack, from October 2024.
  $vbCableUrl = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip'
  $vbCableSha256 = 'b950e39f01af1d04ea623c8f6d8eb9b6ea5c477c637295fabf20631c85116bfb'
  if (-not $VbCablePack) {
    $VbCablePack = "$root\build\vb-cable\$(Split-Path -Leaf $vbCableUrl)"
    if (-not (Test-Path $VbCablePack)) {
      New-Item -ItemType Directory -Force (Split-Path -Parent $VbCablePack) | Out-Null
      Invoke-WebRequest -UseBasicParsing -Uri $vbCableUrl -OutFile "$VbCablePack.part"
      Move-Item -Force "$VbCablePack.part" $VbCablePack
    }
  }
  if (-not (Test-Path $VbCablePack)) { throw "$VbCablePack doesn't exist." }
  $vbCableHash = (Get-FileHash -Algorithm SHA256 $VbCablePack).Hash.ToLower()
  if ($vbCableHash -ne $vbCableSha256) {
    throw "$VbCablePack has SHA-256 $vbCableHash, not $vbCableSha256. If VB-Audio changed its pack, check the new one and update the hash in package.ps1."
  }

  if (-not $Iscc) {
    $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    $Iscc = if ($found) { $found.Source } else {
      @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
  }
  if (-not $Iscc -or -not (Test-Path $Iscc)) { throw "Couldn't find Inno Setup 6's ISCC.exe. Install it, or pass -Iscc." }
  # The ground the wizard image was rendered on, which Setup fills the rest of
  # its area with (tools\render-brand-assets.ps1).
  $sources = Get-Content "$root\assets\brand-sources.txt"
  $ground = $sources | ForEach-Object { if ($_ -match '^scheme\.light\.ground  (#[0-9a-f]{6})$') { $Matches[1] } } |
    Select-Object -First 1
  if (-not $ground) { throw "assets\brand-sources.txt has no scheme.light.ground. Run tools\render-brand-assets.ps1." }
  # KnobsAppId: knobs's own AppId, which a test build without it doesn't get.
  $defines = @("/DAppName=$name", "/DKnobsAppId", "/DAppVersion=$version", "/DStageDir=$stage", "/DOutputDir=$out",
    "/DVbCablePack=$((Resolve-Path $VbCablePack).Path)", "/DWizardGround=$ground")
  if ($Sign) {
    # Inno Setup signs the uninstaller and the installer with this command:
    # $q is a quote and $f the quoted file.
    $defines += '/DSign', ('/Ssigntool=powershell.exe -NoProfile -ExecutionPolicy Bypass -File $q' + $SignScript +
      '$q -Description ' + $name + ' -Url ' + $url + ' $f')
  }
  & $Iscc /Q @defines "$PSScriptRoot\knobs.iss"
  if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed (exit code $LASTEXITCODE)." }
  $artifacts += "$out\$name-$version-setup.exe"
}

$sums = foreach ($file in $artifacts) {
  "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower(), (Split-Path -Leaf $file)
}
[IO.File]::WriteAllText("$out\SHA256SUMS.txt", (($sums -join "`n") + "`n"))
Remove-Item -Recurse -Force "$out\stage", "$out\portable"

foreach ($file in $artifacts + "$out\SHA256SUMS.txt") {
  "{0,-40} {1,8:N1} MB" -f (Split-Path -Leaf $file), ((Get-Item $file).Length / 1MB)
}
