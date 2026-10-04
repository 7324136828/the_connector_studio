[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswherePath -PathType Leaf)) {
    throw 'Install Visual Studio or Build Tools with the Desktop development with C++ workload.'
}
$installationJson = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio discovery failed.' }
$installations = @($installationJson | ConvertFrom-Json)
if ($installations.Count -eq 0) { throw 'No installed MSVC x64 toolchain was found.' }
$installation = $installations[0]
$vsRoot = $installation.installationPath
$vsMajor = ([version]$installation.installationVersion).Major
$toolset = if ($vsMajor -ge 18) { 'v145' } else { 'v143' }
$msbuildPath = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'

function Invoke-NativeCommand {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE." }
}

Write-Host "Building Connector Studio $Configuration x64 with $toolset."
Invoke-NativeCommand $msbuildPath @(
    (Join-Path $repoRoot 'ConnectorStudio.sln'), '/m', '/nologo', '/verbosity:minimal',
    "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:PlatformToolset=$toolset"
)

if ($RunTests) {
    $cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
        if (-not $cmakeCommand) { throw 'Tests require CMake; install the Visual Studio C++ CMake tools component.' }
        $cmakePath = $cmakeCommand.Source
    }
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    $generator = if ($vsMajor -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
    $testBuildRoot = Join-Path $repoRoot 'build\cmake-tests'
    Invoke-NativeCommand $cmakePath @(
        '-S', $repoRoot, '-B', $testBuildRoot, '-G', $generator, '-A', 'x64', '-T', $toolset,
        "-DCMAKE_GENERATOR_INSTANCE=$vsRoot", '-DBUILD_TESTING=ON'
    )
    Invoke-NativeCommand $cmakePath @('--build', $testBuildRoot, '--config', $Configuration, '--target', 'native-tests', '--parallel')
    Invoke-NativeCommand $ctestPath @('--test-dir', $testBuildRoot, '-C', $Configuration, '--output-on-failure')
}

Write-Host "Application: $(Join-Path $repoRoot "build\x64\$Configuration\ConnectorStudio.exe")"
