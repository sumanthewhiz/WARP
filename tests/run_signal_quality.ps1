[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [switch]$NoBuild,
    [string]$Corpus,
    [string]$OutputDir,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EvaluationArgs
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $PSScriptRoot 'WarpTests.vcxproj'
$exe = Join-Path $repo "build\tests\$Platform\Release\WarpTests.exe"

if (-not $NoBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ build tools are required; see tests\README.md.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No Visual Studio C++ installation found.' }
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    $versions = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory |
        Sort-Object { [version]$_.Name } -Descending
    $version = [version]$versions[0].Name
    $toolset = if ($version.Minor -ge 50) { 'v145' } elseif ($version.Minor -ge 30) { 'v143' } else { 'v142' }
    & $msbuild $project /p:Configuration=Release "/p:Platform=$Platform" "/p:PlatformToolset=$toolset" /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$arguments = @((Join-Path $PSScriptRoot 'eval\signal_quality.py'), '--native-bin', $exe)
if ($Corpus) { $arguments += @('--corpus', $Corpus) }
if ($OutputDir) { $arguments += @('--output-dir', $OutputDir) }
if ($EvaluationArgs) { $arguments += $EvaluationArgs }
& python @arguments
exit $LASTEXITCODE
