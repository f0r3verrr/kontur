# Builds dist\Kontur-Setup-<version>.exe and dist\Kontur-<version>-portable.zip.
# Needs: Kontur.exe already built (make / build.bat) and Inno Setup 6 or 7 installed.
param([string]$Version = "1.0.0", [Parameter(Mandatory)][string]$VoiceDir)
$VoiceDir = (Resolve-Path -LiteralPath $VoiceDir -ErrorAction Stop).ProviderPath
$ErrorActionPreference = "Stop"
foreach ($required in 'libvosk.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
    'model/am/final.mdl', 'piper/piper.exe', 'piper/ru_RU-denis-medium.onnx', 'piper/ru_RU-denis-medium.onnx.json') {
    if (-not (Test-Path -LiteralPath (Join-Path $VoiceDir $required) -PathType Leaf)) {
        throw "Voice package is incomplete: $required"
    }
}
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# Bundled HeadsetControl: pinned release, verified against the SHA-256 GitHub publishes for it.
$hscVersion = "4.3.0"
$hscSha256  = "e371da043710ddb8fc70af77f2ccad1cd03f8c6f49ba423e08764ad7a94a3c40"
$hscDir = Join-Path $root "third_party\headsetcontrol"
$hscExe = Join-Path $hscDir "headsetcontrol.exe"
New-Item -ItemType Directory -Force $hscDir | Out-Null
if (-not (Test-Path $hscExe) -or (Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Write-Host "Downloading HeadsetControl $hscVersion..."
    Invoke-WebRequest "https://github.com/Sapd/HeadsetControl/releases/download/$hscVersion/headsetcontrol-windows-x86_64.exe" -OutFile $hscExe
}
if ((Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Remove-Item $hscExe
    throw "HeadsetControl checksum mismatch - refusing to bundle it."
}

if (-not (Test-Path (Join-Path $root "Kontur.exe"))) { throw "Build Kontur.exe first (make or build.bat)." }

$iscc = foreach ($v in 7, 6) {
    foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") { "$base\Inno Setup $v\ISCC.exe" }
}
$iscc = $iscc | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
if (-not $iscc) { throw "Inno Setup not found (https://jrsoftware.org/isdl.php)." }
& $iscc "/DAppVersion=$Version" "/DVoiceDir=$VoiceDir" (Join-Path $PSScriptRoot "Kontur.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC failed." }

# Portable zip: same files, no installer.
$stage = Join-Path $root "build\portable"
if (Test-Path -LiteralPath $stage) {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).ProviderPath
    if ($resolvedStage -ne [IO.Path]::GetFullPath((Join-Path $root 'build/portable'))) {
        throw "Unexpected portable staging path: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item Kontur.exe, $hscExe, installer\THIRD_PARTY_NOTICES.txt $stage
Copy-Item -LiteralPath $VoiceDir -Destination (Join-Path $stage "voice") -Recurse
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
Compress-Archive -Path "$stage\*" -DestinationPath "dist\Kontur-$Version-portable.zip" -Force
Write-Host "Done: dist\Kontur-Setup-$Version.exe and dist\Kontur-$Version-portable.zip"
