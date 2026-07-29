param(
    [Parameter(Mandatory = $true)][int]$PhaseNumber,
    [Parameter(Mandatory = $true)][string]$Title,
    [string]$TrackerPath = "documentation/analysis/progress/phase-$PhaseNumber.json"
)

if (Test-Path -LiteralPath $TrackerPath) { throw "Tracker already exists: $TrackerPath" }
$now = [DateTime]::UtcNow.ToString('o')
$tracker = [ordered]@{
    schema = 'fs2open.telemetry.phase-progress.v2'
    phaseNumber = $PhaseNumber
    title = $Title
    workflowContractVersion = 2
    profile = 'balanced'
    executionState = 'paused'
    updatedAtUtc = $now
    source = [ordered]@{}
    current = [ordered]@{ workPackage=$null; gate=$null; activity='not-started'; summary=''; nextAction='Resolve the first dependency-complete work package.'; startedAtUtc=$null; lastHeartbeatAtUtc=$now }
    campaign = [ordered]@{ id=$null; state='not-started'; completed=0; total=0; unit='scenario'; startedAtUtc=$null; updatedAtUtc=$now; estimatedRemainingMinutes=0 }
    budget = [ordered]@{ limitMinutes=180; plannedMinutes=0; consumedMinutes=0; remainingMinutes=180; exceptionDecisionId=$null }
    statusBuckets = [ordered]@{}
    blockers = @()
    evidence = @()
    nextActions = @('Resolve the first dependency-complete work package.')
    decisionHistory = @()
    notes = @()
}
$parent = Split-Path -Parent $TrackerPath
if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
$tracker | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $TrackerPath -Encoding UTF8
Write-Output "CREATED $TrackerPath"
