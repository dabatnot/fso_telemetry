[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 99)]
    [int]$PhaseNumber,

    [string]$PhaseDirectory,

    [string]$ProgressPath,

    [string]$AnalysisDirectory = 'documentation/analysis',

    [switch]$Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$inspectScript = Join-Path $PSScriptRoot 'inspect_phase_contract.ps1'
if (-not (Test-Path -LiteralPath $inspectScript -PathType Leaf)) {
    throw "Contract inventory script is missing: $inspectScript"
}

$inspectArguments = @{
    PhaseNumber = $PhaseNumber
    AnalysisDirectory = $AnalysisDirectory
    AsJson = $true
}
if (-not [string]::IsNullOrWhiteSpace($PhaseDirectory)) {
    $inspectArguments.PhaseDirectory = $PhaseDirectory
}

$inventoryJson = & $inspectScript @inspectArguments
$inventory = $inventoryJson | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($ProgressPath)) {
    $ProgressPath = Join-Path $AnalysisDirectory "progress/phase-$PhaseNumber.json"
}

$progressFullPath = [System.IO.Path]::GetFullPath($ProgressPath)
if ((Test-Path -LiteralPath $progressFullPath) -and -not $Force) {
    throw "Progress tracker already exists: $progressFullPath"
}

$progressDirectory = Split-Path -Parent $progressFullPath
if (-not (Test-Path -LiteralPath $progressDirectory -PathType Container)) {
    [void](New-Item -ItemType Directory -Path $progressDirectory)
}

$readmePath = Join-Path $inventory.PhaseDirectory 'README.md'
$title = "Phase $PhaseNumber"
if (Test-Path -LiteralPath $readmePath -PathType Leaf) {
    $readmeText = [System.IO.File]::ReadAllText($readmePath, $utf8NoBom)
    $titleMatch = [regex]::Match($readmeText, '(?m)^#\s+(?<title>.+?)\s*$')
    if ($titleMatch.Success) {
        $title = $titleMatch.Groups['title'].Value.Trim()
    }
}

function New-ItemBuckets {
    param([string[]]$PendingIds)

    return [ordered]@{
        pending = @($PendingIds)
        in_progress = @()
        implemented = @()
        verified = @()
        blocked = @()
        deferred = @()
        suspended = @()
    }
}

function New-GateBuckets {
    param([string[]]$PendingIds)

    return [ordered]@{
        pending = @($PendingIds)
        ready = @()
        closed = @()
        reopened = @()
        blocked = @()
    }
}

$allContractItems = @($inventory.RequirementIds)
$testIds = @($allContractItems | Where-Object { $_ -match "-TST-" })
$requirementIds = @($allContractItems | Where-Object { $_ -notmatch "-TST-" })
$workPackageIds = @($inventory.WorkPackageIds)
$gateIds = @($inventory.GateIds)
$acceptanceIds = @($inventory.AcceptanceCriterionIds)

$branch = (& git symbolic-ref --short HEAD 2>$null)
$head = (& git rev-parse HEAD 2>$null)
$trackedStatus = @(& git status --porcelain --untracked-files=no 2>$null)
$worktreeState = if ($trackedStatus.Count -eq 0) { 'clean' } else { 'dirty' }

$tracker = [ordered]@{
    schema = 'fs2open.telemetry.phase-progress.v1'
    phaseNumber = $PhaseNumber
    title = $title
    profile = 'balanced'
    executionState = 'active'
    updatedAtUtc = [DateTime]::UtcNow.ToString('o')
    source = [ordered]@{
        branch = "$branch"
        head = "$head"
        trackedWorktree = $worktreeState
        contractDirectory = [System.IO.Path]::GetFullPath($inventory.PhaseDirectory)
    }
    current = [ordered]@{
        workPackage = if ($workPackageIds.Count -gt 0) { $workPackageIds[0] } else { $null }
        slice = $null
        lastClosedGate = $null
        nextGate = if ($gateIds.Count -gt 0) { $gateIds[0] } else { $null }
        summary = 'Tracker initialized; implementation status has not been reconciled.'
    }
    statusBuckets = [ordered]@{
        workPackages = New-ItemBuckets $workPackageIds
        requirements = New-ItemBuckets $requirementIds
        tests = New-ItemBuckets $testIds
        acceptanceCriteria = New-ItemBuckets $acceptanceIds
        gates = New-GateBuckets $gateIds
    }
    decisionIds = @($inventory.DecisionIds)
    blockers = @()
    evidence = @()
    nextActions = @('Reconcile the tracker against the current implementation and evidence.')
    notes = @()
}

$json = $tracker | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText($progressFullPath, $json + [Environment]::NewLine, $utf8NoBom)

Write-Output "Initialized Phase $PhaseNumber progress tracker."
Write-Output "Path: $progressFullPath"
Write-Output "Work packages: $($workPackageIds.Count)"
Write-Output "Requirements: $($requirementIds.Count)"
Write-Output "Tests: $($testIds.Count)"
Write-Output "Acceptance criteria: $($acceptanceIds.Count)"
Write-Output "Gates: $($gateIds.Count)"
