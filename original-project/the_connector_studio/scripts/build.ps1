[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$RunTests,
    [ValidateRange(1, 32)]
    [int]$MaxParallelJobs = 2,
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildOutputRoot = Join-Path $repoRoot 'build\x64'
if ($OutputRoot) {
    $requestedOutputRoot = if ([System.IO.Path]::IsPathRooted($OutputRoot)) { $OutputRoot } else { Join-Path $repoRoot $OutputRoot }
    $buildOutputRoot = [System.IO.Path]::GetFullPath($requestedOutputRoot).TrimEnd('\', '/')
    $repoPrefix = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $buildOutputRoot.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'OutputRoot must be a directory inside this repository.'
    }
}
$configurationOutputRoot = Join-Path $buildOutputRoot $Configuration
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

Write-Host "Building Connector Studio $Configuration x64 with $toolset (up to $MaxParallelJobs parallel jobs)."
$applicationBuildArguments = @(
    (Join-Path $repoRoot 'ConnectorStudio.sln'), "/m:$MaxParallelJobs", '/nologo', '/verbosity:minimal',
    '/p:MultiProcessorCompilation=false',
    "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:PlatformToolset=$toolset"
)
if ($OutputRoot) {
    $applicationBuildArguments += "/p:OutDir=$configurationOutputRoot\"
    $applicationBuildArguments += "/p:IntDir=$configurationOutputRoot\obj\ConnectorStudio\"
}
Invoke-NativeCommand $msbuildPath $applicationBuildArguments

if ($RunTests) {
    $cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmakePath -PathType Leaf)) {
        $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
        if (-not $cmakeCommand) { throw 'Tests require CMake; install the Visual Studio C++ CMake tools component.' }
        $cmakePath = $cmakeCommand.Source
    }
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    $generator = if ($vsMajor -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
    $testBuildRoot = if ($OutputRoot) { Join-Path $buildOutputRoot 'cmake-tests' } else { Join-Path $repoRoot 'build\cmake-tests' }
    $configureArguments = @(
        '-S', $repoRoot, '-B', $testBuildRoot, '-G', $generator, '-A', 'x64', '-T', $toolset,
        "-DCMAKE_GENERATOR_INSTANCE=$vsRoot", '-DBUILD_TESTING=ON'
    )
    if ($OutputRoot) {
        $runtimeDirectory = $buildOutputRoot.Replace('\', '/') + '/$<CONFIG>'
        $configureArguments += "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$runtimeDirectory"
    }
    Invoke-NativeCommand $cmakePath $configureArguments
    Invoke-NativeCommand $cmakePath @('--build', $testBuildRoot, '--config', $Configuration, '--target', 'native-tests', '--parallel', "$MaxParallelJobs", '--', '/p:MultiProcessorCompilation=false')
    Invoke-NativeCommand $ctestPath @('--test-dir', $testBuildRoot, '-C', $Configuration, '--output-on-failure')
}

Write-Host "Application: $(Join-Path $configurationOutputRoot 'ConnectorStudio.exe')"
