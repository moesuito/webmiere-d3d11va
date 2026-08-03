param(
    [string]$FfmpegSdk,
    [string]$FfmpegSourceBundle = 'C:\Antigravity\SDKs\ffmpeg-webmiere-8.1.2-4-source'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stagingRoot = Join-Path $tempRoot "WebMiere-installer-$([guid]::NewGuid().ToString('N'))"
$payloadRoot = Join-Path $stagingRoot 'payload'
$licenseRoot = Join-Path $payloadRoot 'assets\licenses'
$sourceStage = Join-Path $stagingRoot 'corresponding-source'
$distRoot = Join-Path $repoRoot 'dist'

if (-not $FfmpegSdk) {
    $FfmpegSdk = Join-Path (Split-Path $repoRoot -Parent) 'ffmpeg'
}
$FfmpegSdk = (Resolve-Path $FfmpegSdk).Path
$FfmpegSourceBundle = (Resolve-Path $FfmpegSourceBundle).Path

$iscc = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
if (-not $iscc) {
    throw 'Inno Setup 6 (ISCC.exe) was not found.'
}

$premiere = Get-Process -Name 'Adobe Premiere Pro' -ErrorAction SilentlyContinue
if ($premiere) {
    throw "Adobe Premiere Pro is running (PID $($premiere.Id -join ', ')). Close it before packaging."
}

try {
    New-Item -ItemType Directory -Path (
        Join-Path $payloadRoot 'ffmpeg'),
        (Join-Path $licenseRoot 'source'),
        (Join-Path $sourceStage 'source'),
        $distRoot -Force | Out-Null

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild) {
        throw 'MSBuild was not found.'
    }
    & $msbuild (Join-Path $repoRoot 'WebMiere.sln') /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }

    Copy-Item (Join-Path $repoRoot 'build\x64\Release\WebMiere.prm') $payloadRoot
    foreach ($dll in 'avcodec-62.dll','avformat-62.dll','avutil-60.dll','swresample-6.dll','swscale-9.dll') {
        Copy-Item (Join-Path $FfmpegSdk "bin\$dll") (Join-Path $payloadRoot 'ffmpeg')
    }
    Copy-Item (Join-Path $repoRoot 'ARTWORK_POLICY.md') $licenseRoot
    Copy-Item (Join-Path $repoRoot 'LICENSE') (Join-Path $licenseRoot 'WebMiere-MPL-2.0.txt')
    Copy-Item (Join-Path $repoRoot 'README.md') $licenseRoot
    Copy-Item (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') $licenseRoot
    Copy-Item (Join-Path $FfmpegSourceBundle 'COPYING.LGPLv3') (Join-Path $licenseRoot 'FFmpeg-COPYING.LGPLv3.txt')
    Copy-Item (Join-Path $FfmpegSourceBundle 'COPYING.dav1d') (Join-Path $licenseRoot 'dav1d-COPYING.BSD-2-Clause.txt')

    Copy-Item (Join-Path $FfmpegSourceBundle 'source\ffmpeg-8.1.2-38b88335f99e76ed89ff3c93f877fdefce736c13.tar.gz') (Join-Path $sourceStage 'source')
    Copy-Item (Join-Path $FfmpegSourceBundle 'source\dav1d-1.5.1-42b2b24fb8819f1ed3643aa9cf2a62f03868e3aa.tar.gz') (Join-Path $sourceStage 'source')
    Copy-Item (Join-Path $repoRoot 'packaging\ffmpeg\README.md') $sourceStage
    Copy-Item (Join-Path $repoRoot 'packaging\ffmpeg\configure.args.txt') $sourceStage
    Copy-Item (Join-Path $FfmpegSourceBundle 'COPYING.LGPLv3') $sourceStage
    Copy-Item (Join-Path $FfmpegSourceBundle 'COPYING.dav1d') $sourceStage

    $sourceZip = Join-Path $licenseRoot 'source\WebMiere-D3D11VA-FFmpeg-8.1.2-Corresponding-Source.zip'
    Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $sourceZip -CompressionLevel Optimal
    $releaseSourceZip = Join-Path $distRoot 'WebMiere-D3D11VA-FFmpeg-8.1.2-Corresponding-Source.zip'
    Copy-Item $sourceZip $releaseSourceZip -Force

    $hashLines = Get-ChildItem $payloadRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($payloadRoot.Length + 1).Replace('\', '/')
        "{0} *{1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    }
    [IO.File]::WriteAllLines((Join-Path $licenseRoot 'SHA256SUMS.txt'), $hashLines, [Text.Encoding]::ASCII)

    & $iscc "/DPayloadDir=$payloadRoot" "/DOutputDir=$distRoot" (Join-Path $PSScriptRoot 'WebMiere-D3D11VA.iss')
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup failed with exit code $LASTEXITCODE."
    }

    $installer = Join-Path $distRoot 'WebMiere-D3D11VA-Setup-1.3.0.exe'
    if (-not (Test-Path $installer -PathType Leaf)) {
        throw "Installer was not created: $installer"
    }
    $releaseHashLines = $installer, $releaseSourceZip | ForEach-Object {
        "{0} *{1}" -f (
            (Get-FileHash $_ -Algorithm SHA256).Hash.ToLowerInvariant()), (
            Split-Path $_ -Leaf)
    }
    [IO.File]::WriteAllLines(
        (Join-Path $distRoot 'SHA256SUMS.txt'),
        $releaseHashLines,
        [Text.Encoding]::ASCII)
    Write-Host "Installer: $installer"
    Write-Host "SHA-256: $((Get-FileHash $installer -Algorithm SHA256).Hash.ToLowerInvariant())"
}
finally {
    if (Test-Path $stagingRoot) {
        $resolved = (Resolve-Path $stagingRoot).Path
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Split-Path $resolved -Leaf).StartsWith('WebMiere-installer-', [StringComparison]::Ordinal)) {
            throw "Refusing to remove unexpected staging path: $resolved"
        }
        Remove-Item $resolved -Recurse -Force
    }
}
