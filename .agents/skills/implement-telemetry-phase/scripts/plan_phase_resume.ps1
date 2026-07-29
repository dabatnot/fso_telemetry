param(
    [Parameter(Mandatory = $true)][int]$PhaseNumber,
    [string]$TrackerPath = "documentation/analysis/progress/phase-$PhaseNumber.json"
)

& (Join-Path $PSScriptRoot 'validate_phase_progress.ps1') -PhaseNumber $PhaseNumber -TrackerPath $TrackerPath | Out-Null
$tracker = Get-Content -LiteralPath $TrackerPath -Raw -Encoding UTF8 | ConvertFrom-Json
$active = @($tracker.blockers | Where-Object state -In @('active','open','blocked'))

if ($tracker.executionState -eq 'complete') {
    Write-Output 'NO_ACTION phase already complete'
    exit 0
}
if ($active.Count -gt 0) {
    $blocker = $active[0]
    Write-Output "RESUME_TARGET wp=$($tracker.current.workPackage) gate=$($tracker.current.gate)"
    Write-Output "BLOCKER id=$($blocker.id) category=$($blocker.category) failures=$($blocker.consecutiveFailures)"
    if ([string]::IsNullOrWhiteSpace([string]$blocker.nextAction)) { throw "Active blocker $($blocker.id) has no nextAction" }
    Write-Output "FIRST_ACTION $($blocker.nextAction)"
    Write-Output 'POLICY run the narrow reproducer; do not restart the full campaign while it is red'
    exit 0
}

if ([string]::IsNullOrWhiteSpace([string]$tracker.current.nextAction)) { throw 'Tracker current.nextAction is empty' }
Write-Output "RESUME_TARGET wp=$($tracker.current.workPackage) gate=$($tracker.current.gate)"
Write-Output "FIRST_ACTION $($tracker.current.nextAction)"
