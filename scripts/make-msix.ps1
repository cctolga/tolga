# Packs the Windows program folder (from Tolga-Player-Win.zip) as an MSIX for the Microsoft
# Store. Runs on Windows with the Windows SDK (the GitHub windows runners have it):
#
#   scripts/make-msix.ps1 -Zip dist/Tolga-Player-Win.zip -IdentityName 12345Tolga.TolgaPlayer `
#       -Publisher "CN=XXXXXXXX-XXXX-..." -PublisherName "Tolga Ç."
#
# The three identity values are on Partner Center > the app > Product identity. The package is
# not signed: the Store signs it. (To try it on a PC first, sign it with a test certificate.)
param(
    [Parameter(Mandatory = $true)] [string]$Zip,
    [Parameter(Mandatory = $true)] [string]$IdentityName,
    [Parameter(Mandatory = $true)] [string]$Publisher,
    [Parameter(Mandatory = $true)] [string]$PublisherName,
    [string]$Out = "dist/Tolga-Player-Win.msix"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# x.y.z from CMakeLists; the Store wants four parts and the last one 0
$m = Select-String -Path "$root/CMakeLists.txt" -Pattern 'project\(Tolga VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
if (-not $m) { throw "no version in CMakeLists.txt" }
$version = $m.Matches[0].Groups[1].Value + ".0"

# the newest SDK tools
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$sdk = Get-ChildItem $kits -Directory | Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" } |
       Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $sdk) { throw "Windows SDK (makeappx.exe) not found under $kits" }
$makeappx = "$($sdk.FullName)\x64\makeappx.exe"
$makepri = "$($sdk.FullName)\x64\makepri.exe"

$work = Join-Path ([IO.Path]::GetTempPath()) "tolga-msix"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null
Expand-Archive $Zip -DestinationPath "$work/zip"
# the zip holds one folder, "Tolga Player"
$stage = Get-ChildItem "$work/zip" -Directory | Select-Object -First 1
if (-not $stage -or -not (Test-Path "$($stage.FullName)/Tolga Player.exe")) { throw "Tolga Player.exe not found in $Zip" }
$stage = $stage.FullName

Copy-Item "$root/resources/windows/msix/Assets" "$stage/Assets" -Recurse
$manifest = Get-Content "$root/resources/windows/msix/AppxManifest.xml.in" -Raw
$manifest = $manifest.Replace("@IDENTITY_NAME@", $IdentityName).Replace("@PUBLISHER@", $Publisher).
                      Replace("@PUBLISHER_NAME@", [Security.SecurityElement]::Escape($PublisherName)).
                      Replace("@VERSION@", $version)
Set-Content "$stage/AppxManifest.xml" $manifest -Encoding UTF8

# resources.pri: lets Windows pick the logo size that fits the screen (scale-100/-200, targetsize-*)
Push-Location $stage
& $makepri createconfig /cf "$work/priconfig.xml" /dq en-US /o | Out-Host
& $makepri new /pr $stage /cf "$work/priconfig.xml" /mn "$stage/AppxManifest.xml" /of "$stage/resources.pri" /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "makepri failed" }
Pop-Location

New-Item -ItemType Directory (Split-Path -Parent $Out) -Force | Out-Null
& $makeappx pack /d $stage /p $Out /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }
Write-Host "MSIX $version -> $Out"
