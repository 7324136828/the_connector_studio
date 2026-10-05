[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$IncludeSymbols,
    [ValidateRange(1, 32)]
    [int]$MaxParallelJobs = 2,
    [string]$ExecutablePath = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ($ExecutablePath -and -not $SkipBuild) { throw 'Use -SkipBuild when packaging an explicit ExecutablePath.' }
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -RunTests -MaxParallelJobs $MaxParallelJobs
}

$exePath = if ($ExecutablePath) {
    $requestedExePath = if ([System.IO.Path]::IsPathRooted($ExecutablePath)) { $ExecutablePath } else { Join-Path $repoRoot $ExecutablePath }
    [System.IO.Path]::GetFullPath($requestedExePath)
} else { Join-Path $repoRoot 'build\x64\Release\ConnectorStudio.exe' }
$repoPrefix = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $exePath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'ExecutablePath must be inside this repository.'
}
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw 'Build Release before packaging with -SkipBuild.'
}
$version = (Get-Item -LiteralPath $exePath).VersionInfo.ProductVersion
if (-not $version) { throw 'The executable is missing its version resource.' }

# A fresh staging directory prevents stale build files entering the ZIP.
$stageRoot = Join-Path $repoRoot ("build\package\" + [guid]::NewGuid().ToString('N'))
$packageRoot = Join-Path $stageRoot 'ConnectorStudio'
$distributionRoot = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Path $packageRoot, $distributionRoot -Force | Out-Null
Copy-Item -LiteralPath $exePath -Destination (Join-Path $packageRoot 'ConnectorStudio.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt') -Destination $packageRoot
if ($IncludeSymbols) {
    $symbolsPath = Join-Path (Split-Path -Parent $exePath) 'ConnectorStudio.pdb'
    if (Test-Path -LiteralPath $symbolsPath -PathType Leaf) {
        Copy-Item -LiteralPath $symbolsPath -Destination $packageRoot
    }
}

$manifest = [ordered]@{
    product = 'Connector Studio'
    version = $version
    architecture = 'x64'
    operatingSystem = 'Windows 10 version 1703 or newer / Windows 11'
    executable = 'ConnectorStudio.exe'
    executableSha256 = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
}
$manifestJson = $manifest | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $packageRoot 'manifest.json'), $manifestJson, [System.Text.UTF8Encoding]::new($false))
$archivePath = Join-Path $distributionRoot "ConnectorStudio-$version-win-x64.zip"
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -Force
$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
"$archiveHash  $(Split-Path -Leaf $archivePath)" | Set-Content -LiteralPath "$archivePath.sha256" -Encoding ASCII
Write-Host "Portable package: $archivePath"
Write-Host "SHA-256: $archiveHash"
