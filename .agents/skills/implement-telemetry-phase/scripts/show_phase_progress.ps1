param(
    [Parameter(Mandatory = $true)][int]$PhaseNumber,
    [string]$TrackerPath = "documentation/analysis/progress/phase-$PhaseNumber.json"
)

& (Join-Path $PSScriptRoot 'validate_phase_progress.ps1') -PhaseNumber $PhaseNumber -TrackerPath $TrackerPath | Out-Null
$tracker = Get-Content -LiteralPath $TrackerPath -Raw -Encoding UTF8 | ConvertFrom-Json
[pscustomobject]@{
    Phase = $tracker.phaseNumber
    State = $tracker.executionState
    WorkPackage = $tracker.current.workPackage
    Gate = $tracker.current.gate
    Activity = $tracker.current.activity
    Progress = "$($tracker.campaign.completed)/$($tracker.campaign.total) $($tracker.campaign.unit)"
    Budget = "$($tracker.budget.remainingMinutes)/$($tracker.budget.limitMinutes) min remaining"
    Blocker = (@($tracker.blockers | Where-Object state -In @('active','open','blocked') | Select-Object -ExpandProperty id) -join ',')
    NextAction = $tracker.current.nextAction
    UpdatedAtUtc = $tracker.updatedAtUtc
} | Format-List
