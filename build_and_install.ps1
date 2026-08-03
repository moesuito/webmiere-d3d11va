param([switch]$InstallOnly)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$policySource = Join-Path $repoRoot 'ARTWORK_POLICY.md'
$requiredPolicyHash = 'E48086E75025C38CA67E1C6234185A8A10985BD2E66A39590A5EE60BD0B0B0D7'

if (-not (Test-Path -LiteralPath $policySource -PathType Leaf)) {
    throw "Required policy file not found: $policySource"
}
if ((Get-FileHash -LiteralPath $policySource -Algorithm SHA256).Hash -ne $requiredPolicyHash) {
    throw 'ARTWORK_POLICY.md does not match the SHA-256 required by WebMiere.'
}

$premiere = Get-Process -Name 'Adobe Premiere Pro' -ErrorAction SilentlyContinue
if ($premiere) {
    throw "Adobe Premiere Pro is running (PID $($premiere.Id -join ', ')). Close it before building or installing WebMiere."
}

$pluginSource = Join-Path $repoRoot 'build\x64\Release\WebMiere.prm'
$ffmpegBin = Join-Path (Split-Path -Parent $repoRoot) 'ffmpeg\bin'
$ffmpegDlls = @(
    'avcodec-62.dll'
    'avformat-62.dll'
    'avutil-60.dll'
    'swresample-6.dll'
    'swscale-9.dll'
)

if (-not $InstallOnly) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "vswhere.exe not found: $vswhere"
    }

    $msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw 'MSBuild was not found by vswhere.exe.'
    }

    Write-Host 'Building WebMiere Release x64...'
    & $msbuild (Join-Path $repoRoot 'WebMiere.sln') /m /t:Build /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

$installRoot = Join-Path $env:ProgramFiles 'Adobe\Common\Plug-ins\7.0\MediaCore\WebMiere'
$ffmpegDestination = Join-Path $installRoot 'ffmpeg'
$licenseDestination = Join-Path $installRoot 'assets\licenses'
$files = @(
    [pscustomobject]@{
        Source = $pluginSource
        Destination = Join-Path $installRoot 'WebMiere.prm'
        RelativePath = 'WebMiere.prm'
    }
    $ffmpegDlls | ForEach-Object {
        [pscustomobject]@{
            Source = Join-Path $ffmpegBin $_
            Destination = Join-Path $ffmpegDestination $_
            RelativePath = Join-Path 'ffmpeg' $_
        }
    }
    [pscustomobject]@{
        Source = $policySource
        Destination = Join-Path $licenseDestination 'ARTWORK_POLICY.md'
        RelativePath = 'assets\licenses\ARTWORK_POLICY.md'
    }
)

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath $file.Source -PathType Leaf)) {
        throw "Required build artifact not found: $($file.Source)"
    }
    $file | Add-Member -NotePropertyName SourceHash -NotePropertyValue (
        (Get-FileHash -LiteralPath $file.Source -Algorithm SHA256).Hash
    )
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)
if (-not $isAdmin) {
    if ($InstallOnly) {
        throw 'Administrator elevation did not succeed.'
    }

    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $elevationArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -InstallOnly"
    try {
        $elevated = Start-Process -FilePath $powershell -ArgumentList $elevationArgs -Verb RunAs -Wait -PassThru
    }
    catch {
        throw "Administrator elevation was cancelled or failed: $($_.Exception.Message)"
    }
    if ($elevated.ExitCode -ne 0) {
        throw "Elevated installation failed with exit code $($elevated.ExitCode)."
    }
    exit 0
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stagingRoot = Join-Path $tempRoot "WebMiere-install-$([guid]::NewGuid().ToString('N'))"
$payloadRoot = Join-Path $stagingRoot 'payload'
$backupRoot = Join-Path $stagingRoot 'backup'
$preserveStaging = $false

try {
    New-Item -ItemType Directory -Path $payloadRoot, $backupRoot | Out-Null

    foreach ($file in $files) {
        $file | Add-Member -NotePropertyName Staged -NotePropertyValue (Join-Path $payloadRoot $file.RelativePath)
        $file | Add-Member -NotePropertyName Backup -NotePropertyValue (Join-Path $backupRoot $file.RelativePath)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file.Staged) | Out-Null
        Copy-Item -LiteralPath $file.Source -Destination $file.Staged
        if ((Get-FileHash -LiteralPath $file.Staged -Algorithm SHA256).Hash -ne $file.SourceHash) {
            throw "SHA-256 mismatch in staging: $($file.Staged)"
        }
    }

    foreach ($file in $files) {
        $file | Add-Member -NotePropertyName HadOriginal -NotePropertyValue (
            Test-Path -LiteralPath $file.Destination -PathType Leaf
        )
        if ((Test-Path -LiteralPath $file.Destination) -and -not $file.HadOriginal) {
            throw "Install target exists but is not a file: $($file.Destination)"
        }
        if ($file.HadOriginal) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file.Backup) | Out-Null
            $file | Add-Member -NotePropertyName OriginalHash -NotePropertyValue (
                (Get-FileHash -LiteralPath $file.Destination -Algorithm SHA256).Hash
            )
            Copy-Item -LiteralPath $file.Destination -Destination $file.Backup
            if ((Get-FileHash -LiteralPath $file.Backup -Algorithm SHA256).Hash -ne $file.OriginalHash) {
                throw "SHA-256 mismatch while backing up: $($file.Destination)"
            }
        }
    }

    $premiere = Get-Process -Name 'Adobe Premiere Pro' -ErrorAction SilentlyContinue
    if ($premiere) {
        throw "Adobe Premiere Pro started during installation preflight (PID $($premiere.Id -join ', ')). Close it and retry."
    }
    New-Item -ItemType Directory -Force -Path $installRoot, $ffmpegDestination, $licenseDestination | Out-Null

    try {
        foreach ($file in $files) {
            Copy-Item -LiteralPath $file.Staged -Destination $file.Destination -Force
            if ((Get-FileHash -LiteralPath $file.Destination -Algorithm SHA256).Hash -ne $file.SourceHash) {
                throw "SHA-256 mismatch after copying: $($file.Destination)"
            }
        }
    }
    catch {
        $installError = $_
        $rollbackErrors = @()
        foreach ($file in $files) {
            try {
                if ($file.HadOriginal) {
                    Copy-Item -LiteralPath $file.Backup -Destination $file.Destination -Force
                    if ((Get-FileHash -LiteralPath $file.Destination -Algorithm SHA256).Hash -ne $file.OriginalHash) {
                        throw "SHA-256 mismatch restoring $($file.Destination)"
                    }
                }
                elseif (Test-Path -LiteralPath $file.Destination -PathType Leaf) {
                    Remove-Item -LiteralPath $file.Destination -Force
                }
            }
            catch {
                $rollbackErrors += $_.Exception.Message
            }
        }
        if ($rollbackErrors) {
            $preserveStaging = $true
            throw "$($installError.Exception.Message) Rollback also failed: $($rollbackErrors -join '; '). Recovery files preserved at: $stagingRoot"
        }
        throw $installError
    }

    Write-Host "WebMiere installed and SHA-256 verified: $installRoot"
}
finally {
    if (-not $preserveStaging -and (Test-Path -LiteralPath $stagingRoot)) {
        $resolvedStagingRoot = (Resolve-Path -LiteralPath $stagingRoot).Path
        if (-not $resolvedStagingRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove unexpected staging path: $resolvedStagingRoot"
        }
        foreach ($file in $files) {
            foreach ($propertyName in 'Staged', 'Backup') {
                $property = $file.PSObject.Properties[$propertyName]
                if ($property -and (Test-Path -LiteralPath $property.Value -PathType Leaf)) {
                    Remove-Item -LiteralPath $property.Value -Force
                }
            }
        }
        $cleanupDirectories = @(
            (Join-Path $backupRoot 'assets\licenses'), (Join-Path $backupRoot 'assets'), (Join-Path $backupRoot 'ffmpeg'), $backupRoot
            (Join-Path $payloadRoot 'assets\licenses'), (Join-Path $payloadRoot 'assets'), (Join-Path $payloadRoot 'ffmpeg'), $payloadRoot
            $resolvedStagingRoot
        )
        foreach ($directory in $cleanupDirectories) {
            if (Test-Path -LiteralPath $directory -PathType Container) {
                Remove-Item -LiteralPath $directory -Force
            }
        }
    }
}
