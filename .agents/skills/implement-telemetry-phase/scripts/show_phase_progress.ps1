[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 99)]
    [int]$PhaseNumber,

    [string]$PhaseDirectory,

    [string]$ProgressPath,

    [string]$AnalysisDirectory = 'documentation/analysis'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)

if ([string]::IsNullOrWhiteSpace($ProgressPath)) {
    $ProgressPath = Join-Path $AnalysisDirectory "progress/phase-$PhaseNumber.json"
}

$validator = Join-Path $PSScriptRoot 'validate_phase_progress.ps1'
$validationArguments = @{
    PhaseNumber = $PhaseNumber
    ProgressPath = $ProgressPath
    AnalysisDirectory = $AnalysisDirectory
    Quiet = $true
}
if (-not [string]::IsNullOrWhiteSpace($PhaseDirectory)) {
    $validationArguments.PhaseDirectory = $PhaseDirectory
}
& $validator @validationArguments

$progressFullPath = (Resolve-Path -LiteralPath $ProgressPath).Path
$progress = [System.IO.File]::ReadAllText($progressFullPath, $utf8) | ConvertFrom-Json

function Format-BucketCounts {
    param([object]$Buckets)

    $parts = @()
    foreach ($property in $Buckets.PSObject.Properties) {
        $count = @($property.Value).Count
        if ($count -gt 0) {
            $parts += "$($property.Name)=$count"
        }
    }
    return $parts -join ', '
}

$activeBlockers = @($progress.blockers | Where-Object { $_.state -eq 'active' })
$staleEvidence = @($progress.evidence | Where-Object { $_.status -eq 'stale' })

Write-Output "$($progress.title) - $($progress.executionState.ToString().ToUpperInvariant())"
Write-Output "Profile          : $($progress.profile)"
Write-Output "Current WP       : $($progress.current.workPackage)"
Write-Output "Current slice    : $($progress.current.slice)"
Write-Output "Last closed gate : $($progress.current.lastClosedGate)"
Write-Output "Next gate        : $($progress.current.nextGate)"
Write-Output "Summary          : $($progress.current.summary)"
Write-Output "Work packages    : $(Format-BucketCounts $progress.statusBuckets.workPackages)"
Write-Output "Requirements     : $(Format-BucketCounts $progress.statusBuckets.requirements)"
Write-Output "Tests            : $(Format-BucketCounts $progress.statusBuckets.tests)"
Write-Output "Acceptance       : $(Format-BucketCounts $progress.statusBuckets.acceptanceCriteria)"
Write-Output "Gates            : $(Format-BucketCounts $progress.statusBuckets.gates)"
Write-Output "Active blockers  : $($activeBlockers.Count)"
foreach ($blocker in $activeBlockers) {
    Write-Output "  - $($blocker.id): $($blocker.summary) -> $($blocker.nextProof)"
}
Write-Output "Stale evidence   : $($staleEvidence.Count)"
Write-Output 'Next actions:'
foreach ($action in @($progress.nextActions)) {
    Write-Output "  - $action"
}
