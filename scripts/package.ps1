[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$IncludeSymbols
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -RunTests
}

$exePath = Join-Path $repoRoot 'build\x64\Release\ConnectorStudio.exe'
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
if ($IncludeSymbols) {
    $symbolsPath = Join-Path $repoRoot 'build\x64\Release\ConnectorStudio.pdb'
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
