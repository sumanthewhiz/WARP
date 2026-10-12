<#
.SYNOPSIS
    Downloads and verifies WARP's model inputs (gap analysis SEC-2).

.DESCRIPTION
    Reads scripts/models.lock.json and, for every model:
      * resolves its revision to a commit via the Hugging Face API and downloads each file from
        https://huggingface.co/<repo>/resolve/<commit>/<path> (an immutable URL);
      * verifies every file: against the pinned 'sha256' when present, otherwise against the
        hashes Hugging Face publishes for that commit (LFS SHA-256, or the git blob SHA-1 for small
        files) plus the published size;
      * reports unpinned inputs (branch revisions / empty hashes) - a warning, or a failure with
        -RequirePinned (release builds).
    -UpdateLock writes the resolved commits and computed SHA-256 values back to the lock file.

    PERF-1: the Qwen genai_config.json gets an explicit intra-op thread cap (-QwenThreadCap), so
    the optional refiner can never take every core.

.EXAMPLE
    pwsh scripts/fetch_models.ps1                    # fetch + verify everything
    pwsh scripts/fetch_models.ps1 -Only granite      # one model
    pwsh scripts/fetch_models.ps1 -UpdateLock        # pin the current upstream state
#>
[CmdletBinding()]
param(
    [string]$LockFile = (Join-Path $PSScriptRoot 'models.lock.json'),
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string[]]$Only = @(),
    [switch]$UpdateLock,
    [switch]$RequirePinned,
    [int]$QwenThreadCap = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$headers = @{ 'User-Agent' = 'warp-build (github.com/sumanthewhiz/WARP)' }

function Get-GitBlobSha1([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    try {
        [void]$sha1.TransformBlock($header, 0, $header.Length, $null, 0)
        [void]$sha1.TransformFinalBlock($bytes, 0, $bytes.Length)
        return ([System.BitConverter]::ToString($sha1.Hash) -replace '-', '').ToLowerInvariant()
    }
    finally { $sha1.Dispose() }
}

function Invoke-WithRetry([scriptblock]$Action, [string]$What) {
    for ($attempt = 1; ; $attempt++) {
        try { return & $Action }
        catch {
            if ($attempt -ge 4) { throw "$What failed after $attempt attempts: $_" }
            Write-Host "  retry $attempt for $What ($($_.Exception.Message))"
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
}

function Get-Prop($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $p = $Object.PSObject.Properties[$Name]
    if ($null -eq $p) { return $null }
    return $p.Value
}

$lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json
$unpinned = [System.Collections.Generic.List[string]]::new()

foreach ($m in $lock.models) {
    if ($Only.Count -gt 0 -and $Only -notcontains $m.name) { continue }
    $pinnedCommit = $m.revision -match '^[0-9a-f]{40}$'
    $infoUri = "https://huggingface.co/api/models/$($m.repo)/revision/$($m.revision)?blobs=true"
    $info = Invoke-WithRetry { Invoke-RestMethod -Uri $infoUri -Headers $headers -TimeoutSec 60 } "metadata for $($m.repo)"
    $commit = [string](Get-Prop $info 'sha')
    if (-not $commit) { throw "Hugging Face did not resolve $($m.repo)@$($m.revision)" }
    if ($pinnedCommit -and $commit -ne $m.revision) { throw "$($m.repo): pinned commit $($m.revision) resolved to $commit" }
    if (-not $pinnedCommit) { $unpinned.Add("$($m.name): revision '$($m.revision)' (resolved to $commit)") }
    Write-Host "$($m.name): $($m.repo)@$commit"

    $destDir = Join-Path $Root $m.dest
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    $siblings = @(Get-Prop $info 'siblings')
    foreach ($f in $m.files) {
        $sib = $siblings | Where-Object { $_.rfilename -eq $f.path } | Select-Object -First 1
        if ($null -eq $sib) { throw "$($m.repo)@$commit has no file '$($f.path)'" }
        $out = Join-Path $destDir $f.dest
        $uri = "https://huggingface.co/$($m.repo)/resolve/$commit/$($f.path)"
        Invoke-WithRetry { Invoke-WebRequest -Uri $uri -OutFile $out -Headers $headers -TimeoutSec 1800 | Out-Null } "download of $($f.path)" | Out-Null

        $size = (Get-Item -LiteralPath $out).Length
        $sha256 = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLowerInvariant()
        $publishedSize = Get-Prop $sib 'size'
        if ($publishedSize -and [int64]$publishedSize -ne $size) {
            throw "$($m.name)/$($f.dest): size $size differs from the published $publishedSize"
        }
        if ($f.sha256) {
            if ($sha256 -ne ([string]$f.sha256).ToLowerInvariant()) {
                throw "$($m.name)/$($f.dest): SHA-256 mismatch (expected $($f.sha256), got $sha256)"
            }
        }
        else {
            $lfs = Get-Prop $sib 'lfs'
            $lfsSha = Get-Prop $lfs 'sha256'
            $blobId = Get-Prop $sib 'blobId'
            if ($lfsSha) {
                if ($sha256 -ne ([string]$lfsSha).ToLowerInvariant()) { throw "$($m.name)/$($f.dest): SHA-256 differs from the published LFS hash" }
            }
            elseif ($blobId) {
                if ((Get-GitBlobSha1 $out) -ne ([string]$blobId).ToLowerInvariant()) { throw "$($m.name)/$($f.dest): content differs from the published git blob" }
            }
            else {
                throw "$($m.name)/$($f.dest): Hugging Face published no hash to verify against"
            }
            $unpinned.Add("$($m.name)/$($f.dest): sha256 not pinned ($sha256)")
            if ($UpdateLock) { $f.sha256 = $sha256 }
        }
        Write-Host ("  {0,-28} {1,14:N0} bytes  sha256 {2}" -f $f.dest, $size, $sha256)
    }
    if ($UpdateLock) { $m.revision = $commit }
}

# PERF-1: explicit thread cap for the optional ORT-GenAI refiner.
$genaiConfig = Join-Path $Root 'models/qwen/genai_config.json'
if ($QwenThreadCap -gt 0 -and (Test-Path -LiteralPath $genaiConfig)) {
    $cfg = Get-Content -LiteralPath $genaiConfig -Raw | ConvertFrom-Json
    $decoder = Get-Prop (Get-Prop $cfg 'model') 'decoder'
    if ($null -ne $decoder) {
        if ($null -eq (Get-Prop $decoder 'session_options')) {
            $decoder | Add-Member -NotePropertyName 'session_options' -NotePropertyValue ([pscustomobject]@{})
        }
        $decoder.session_options | Add-Member -NotePropertyName 'intra_op_num_threads' -NotePropertyValue $QwenThreadCap -Force
        $cfg | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath $genaiConfig -Encoding utf8
        Write-Host "genai_config.json: intra_op_num_threads=$QwenThreadCap"
    }
}

if ($UpdateLock) {
    $lock | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $LockFile -Encoding utf8
    Write-Host "Pinned the resolved commits and SHA-256 values in $LockFile"
}
elseif ($unpinned.Count -gt 0) {
    $msg = "Model inputs verified against Hugging Face but not pinned in scripts/models.lock.json:`n  " + ($unpinned -join "`n  ")
    if ($RequirePinned) { throw $msg }
    if ($env:GITHUB_ACTIONS -eq 'true') {
        Write-Host "::warning title=Unpinned model inputs::$($unpinned.Count) model inputs are verified but not pinned; run scripts/fetch_models.ps1 -UpdateLock"
    }
    Write-Warning $msg
}
