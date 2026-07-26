[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 99)]
    [int]$PhaseNumber,

    [string]$PhaseDirectory,

    [string]$ProgressPath,

    [string]$AnalysisDirectory = 'documentation/analysis',

    [switch]$Quiet
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)
$errors = New-Object 'System.Collections.Generic.List[string]'

function Add-ProgressError {
    param([string]$Message)
    $script:errors.Add($Message)
}

function Get-BucketIds {
    param(
        [object]$Buckets,
        [string[]]$AllowedStatuses,
        [string]$Label
    )

    if ($null -eq $Buckets) {
        Add-ProgressError "$Label buckets are missing."
        return @()
    }

    $ids = @()
    foreach ($property in $Buckets.PSObject.Properties) {
        if ($AllowedStatuses -notcontains $property.Name) {
            Add-ProgressError "$Label contains unsupported status '$($property.Name)'."
        }
        $ids += @($property.Value)
    }
    return @($ids)
}

function Test-ExactPartition {
    param(
        [object]$Buckets,
        [string[]]$ExpectedIds,
        [string[]]$AllowedStatuses,
        [string]$Label
    )

    $actualIds = @(Get-BucketIds $Buckets $AllowedStatuses $Label)
    $duplicates = @($actualIds | Group-Object | Where-Object { $_.Count -gt 1 })
    foreach ($duplicate in $duplicates) {
        Add-ProgressError "$Label ID '$($duplicate.Name)' occurs $($duplicate.Count) times."
    }

    $missing = @($ExpectedIds | Where-Object { $actualIds -notcontains $_ })
    $extra = @($actualIds | Where-Object { $ExpectedIds -notcontains $_ })
    if ($missing.Count -gt 0) {
        Add-ProgressError "$Label is missing: $($missing -join ', ')."
    }
    if ($extra.Count -gt 0) {
        Add-ProgressError "$Label contains unknown IDs: $($extra -join ', ')."
    }
}

if ([string]::IsNullOrWhiteSpace($ProgressPath)) {
    $ProgressPath = Join-Path $AnalysisDirectory "progress/phase-$PhaseNumber.json"
}
if (-not (Test-Path -LiteralPath $ProgressPath -PathType Leaf)) {
    throw "Progress tracker does not exist: $ProgressPath"
}

$progressFullPath = (Resolve-Path -LiteralPath $ProgressPath).Path
try {
    $progressText = [System.IO.File]::ReadAllText($progressFullPath, $utf8)
    $progress = $progressText | ConvertFrom-Json
}
catch {
    throw "Invalid UTF-8 or JSON progress tracker: $progressFullPath"
}

$inspectScript = Join-Path $PSScriptRoot 'inspect_phase_contract.ps1'
$inspectArguments = @{
    PhaseNumber = $PhaseNumber
    AnalysisDirectory = $AnalysisDirectory
    AsJson = $true
}
if (-not [string]::IsNullOrWhiteSpace($PhaseDirectory)) {
    $inspectArguments.PhaseDirectory = $PhaseDirectory
}
$inventory = (& $inspectScript @inspectArguments) | ConvertFrom-Json

if ($progress.schema -ne 'fs2open.telemetry.phase-progress.v1') {
    Add-ProgressError "Unsupported schema '$($progress.schema)'."
}
if ([int]$progress.phaseNumber -ne $PhaseNumber) {
    Add-ProgressError "Tracker phase '$($progress.phaseNumber)' does not match requested phase $PhaseNumber."
}
if (@('balanced', 'certification') -notcontains $progress.profile) {
    Add-ProgressError "Unsupported profile '$($progress.profile)'."
}
if (@('active', 'paused', 'blocked', 'certifying', 'complete') -notcontains $progress.executionState) {
    Add-ProgressError "Unsupported execution state '$($progress.executionState)'."
}
try {
    [void][DateTime]::Parse($progress.updatedAtUtc)
}
catch {
    Add-ProgressError "updatedAtUtc is not a valid timestamp."
}

$contractItems = @($inventory.RequirementIds)
$testIds = @($contractItems | Where-Object { $_ -match '-TST-' })
$requirementIds = @($contractItems | Where-Object { $_ -notmatch '-TST-' })
$itemStatuses = @('pending', 'in_progress', 'implemented', 'verified', 'blocked', 'deferred', 'suspended')
$gateStatuses = @('pending', 'ready', 'closed', 'reopened', 'blocked')

Test-ExactPartition $progress.statusBuckets.workPackages @($inventory.WorkPackageIds) $itemStatuses 'workPackages'
Test-ExactPartition $progress.statusBuckets.requirements $requirementIds $itemStatuses 'requirements'
Test-ExactPartition $progress.statusBuckets.tests $testIds $itemStatuses 'tests'
Test-ExactPartition $progress.statusBuckets.acceptanceCriteria @($inventory.AcceptanceCriterionIds) $itemStatuses 'acceptanceCriteria'
Test-ExactPartition $progress.statusBuckets.gates @($inventory.GateIds) $gateStatuses 'gates'

$decisionIds = @($progress.decisionIds)
$missingDecisions = @($inventory.DecisionIds | Where-Object { $decisionIds -notcontains $_ })
$extraDecisions = @($decisionIds | Where-Object { @($inventory.DecisionIds) -notcontains $_ })
if ($missingDecisions.Count -gt 0 -or $extraDecisions.Count -gt 0) {
    Add-ProgressError 'decisionIds does not exactly match the contract inventory.'
}

if ($null -ne $progress.current.workPackage -and
    @($inventory.WorkPackageIds) -notcontains $progress.current.workPackage) {
    Add-ProgressError "Unknown current work package '$($progress.current.workPackage)'."
}
if ($null -ne $progress.current.lastClosedGate -and
    @($progress.statusBuckets.gates.closed) -notcontains $progress.current.lastClosedGate) {
    Add-ProgressError "lastClosedGate '$($progress.current.lastClosedGate)' is not in the closed gate bucket."
}
if ($null -ne $progress.current.nextGate -and
    @($inventory.GateIds) -notcontains $progress.current.nextGate) {
    Add-ProgressError "Unknown next gate '$($progress.current.nextGate)'."
}

$allEvidence = @($progress.evidence)
$freshCoverage = @(
    $allEvidence |
        Where-Object { $_.status -eq 'fresh' } |
        ForEach-Object { @($_.covers) }
)
$verifiedIds = @(
    @($progress.statusBuckets.workPackages.verified) +
    @($progress.statusBuckets.requirements.verified) +
    @($progress.statusBuckets.tests.verified) +
    @($progress.statusBuckets.acceptanceCriteria.verified)
)
foreach ($verifiedId in $verifiedIds) {
    if ($freshCoverage -notcontains $verifiedId) {
        Add-ProgressError "Verified ID '$verifiedId' has no fresh evidence coverage."
    }
}

foreach ($gateId in @($progress.statusBuckets.gates.closed)) {
    $gateEvidence = @(
        $allEvidence |
            Where-Object { $_.status -eq 'fresh' -and @($_.covers) -contains $gateId }
    )
    if ($gateEvidence.Count -eq 0) {
        Add-ProgressError "Closed gate '$gateId' has no fresh evidence record."
        continue
    }
    foreach ($record in $gateEvidence) {
        if (-not [string]::IsNullOrWhiteSpace($record.path) -and
            -not (Test-Path -LiteralPath $record.path -PathType Leaf)) {
            Add-ProgressError "Evidence '$($record.id)' points to missing path '$($record.path)'."
        }
    }
}

foreach ($blocker in @($progress.blockers)) {
    if (@('active', 'resolved_unverified', 'closed') -notcontains $blocker.state) {
        Add-ProgressError "Blocker '$($blocker.id)' has unsupported state '$($blocker.state)'."
    }
    if ($blocker.state -eq 'active' -and [string]::IsNullOrWhiteSpace($blocker.nextProof)) {
        Add-ProgressError "Active blocker '$($blocker.id)' has no nextProof."
    }
}

if ($progress.executionState -eq 'complete' -and
    (@($progress.statusBuckets.workPackages.verified).Count -ne @($inventory.WorkPackageIds).Count -or
     @($progress.statusBuckets.gates.closed).Count -ne @($inventory.GateIds).Count)) {
    Add-ProgressError 'Execution state is complete but not every work package and gate is verified/closed.'
}

if ($errors.Count -gt 0) {
    $message = "Phase progress validation failed with $($errors.Count) error(s):`n- " +
        ($errors -join "`n- ")
    throw $message
}

if (-not $Quiet) {
    Write-Output "Phase $PhaseNumber progress validation passed."
    Write-Output "Path: $progressFullPath"
    Write-Output "Execution: $($progress.executionState)"
    Write-Output "Current WP: $($progress.current.workPackage)"
    Write-Output "Last closed gate: $($progress.current.lastClosedGate)"
    Write-Output "Next gate: $($progress.current.nextGate)"
}
