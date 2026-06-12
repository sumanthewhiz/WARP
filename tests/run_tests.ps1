<#
.SYNOPSIS
  PowerShell wrapper around tests/run_tests.py.

.DESCRIPTION
  Convenience wrapper so contributors on Windows can run the suite
  via `.\tests\run_tests.ps1` without remembering the python -m
  pytest invocation.

  Examples:
    .\tests\run_tests.ps1                   # everything
    .\tests\run_tests.ps1 -Layer l1         # L1 only
    .\tests\run_tests.ps1 -Layer l2 -Verbose
    .\tests\run_tests.ps1 -List             # list discovered tests

.PARAMETER Layer
  Filter to one layer: l1, l1_property, l2, l3.
#>
[CmdletBinding()]
param(
    [ValidateSet('l1', 'l1_property', 'l2', 'l3')]
    [string]$Layer,

    [ValidateSet('python', 'scripts', 'all')]
    [string]$Tier = 'all',

    [switch]$List,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$PytestArgs
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
$python = (Get-Command python -ErrorAction Stop).Source

# First-time setup: ensure pytest + hypothesis are available.
$req = Join-Path $here "requirements.txt"
& $python -m pip install --quiet --disable-pip-version-check -r $req
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to install test requirements from $req"
    exit $LASTEXITCODE
}

$args = @((Join-Path $here "run_tests.py"))
if ($Layer) { $args += @("--layer", $Layer) }
if ($Tier)  { $args += @("--tier",  $Tier)  }
if ($List)  { $args += @("--list") }
if ($PytestArgs) { $args += $PytestArgs }

& $python @args
exit $LASTEXITCODE
