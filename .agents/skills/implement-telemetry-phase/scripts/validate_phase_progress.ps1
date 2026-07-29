param(
    [Parameter(Mandatory = $true)][int]$PhaseNumber,
    [string]$TrackerPath = "documentation/analysis/progress/phase-$PhaseNumber.json"
)

$ErrorActionPreference = 'Stop'
$errors = [System.Collections.Generic.List[string]]::new()

if (-not (Test-Path -LiteralPath $TrackerPath -PathType Leaf)) {
    throw "Tracker not found: $TrackerPath"
}

$tracker = Get-Content -LiteralPath $TrackerPath -Raw -Encoding UTF8 | ConvertFrom-Json
$required = @('schema','phaseNumber','title','workflowContractVersion','profile','executionState','updatedAtUtc','source','current','campaign','budget','statusBuckets','blockers','evidence','nextActions','decisionHistory','notes')
foreach ($name in $required) {
    if (-not ($tracker.PSObject.Properties.Name -contains $name)) {
        $errors.Add("Missing root property: $name")
    }
}

if ($tracker.schema -ne 'fs2open.telemetry.phase-progress.v2') { $errors.Add("Unexpected schema: $($tracker.schema)") }
if ($tracker.phaseNumber -ne $PhaseNumber) { $errors.Add("phaseNumber does not match $PhaseNumber") }
if ($tracker.workflowContractVersion -ne 2) { $errors.Add('workflowContractVersion must be 2') }
if ($tracker.executionState -notin @('running','paused','blocked','complete')) { $errors.Add("Invalid executionState: $($tracker.executionState)") }

foreach ($name in @('workPackage','gate','activity','summary','nextAction','startedAtUtc','lastHeartbeatAtUtc')) {
    if (-not ($tracker.current.PSObject.Properties.Name -contains $name)) { $errors.Add("Missing current.$name") }
}
foreach ($name in @('id','state','completed','total','unit','startedAtUtc','updatedAtUtc','estimatedRemainingMinutes')) {
    if (-not ($tracker.campaign.PSObject.Properties.Name -contains $name)) { $errors.Add("Missing campaign.$name") }
}
if ($tracker.campaign.completed -lt 0 -or $tracker.campaign.total -lt 0 -or $tracker.campaign.completed -gt $tracker.campaign.total) {
    $errors.Add('Campaign progress must satisfy 0 <= completed <= total')
}
if ($tracker.campaign.state -notin @('not-started','qualified','running','paused','failed','complete')) {
    $errors.Add("Invalid campaign.state: $($tracker.campaign.state)")
}
foreach ($name in @('limitMinutes','plannedMinutes','consumedMinutes','remainingMinutes','exceptionDecisionId')) {
    if (-not ($tracker.budget.PSObject.Properties.Name -contains $name)) { $errors.Add("Missing budget.$name") }
}
if ($tracker.budget.limitMinutes -gt 180 -and [string]::IsNullOrWhiteSpace([string]$tracker.budget.exceptionDecisionId)) {
    $errors.Add('Budget exceeds 180 minutes without exceptionDecisionId')
}

$activeBlockers = @($tracker.blockers | Where-Object state -In @('active','open','blocked'))
foreach ($blocker in $activeBlockers) {
    foreach ($name in @('id','category','owner','summary','affects','nextAction','consecutiveFailures')) {
        if (-not ($blocker.PSObject.Properties.Name -contains $name)) { $errors.Add("Active blocker is missing $name") }
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output "PASS phase=$PhaseNumber state=$($tracker.executionState) wp=$($tracker.current.workPackage) gate=$($tracker.current.gate) progress=$($tracker.campaign.completed)/$($tracker.campaign.total) budgetRemaining=$($tracker.budget.remainingMinutes)m"
