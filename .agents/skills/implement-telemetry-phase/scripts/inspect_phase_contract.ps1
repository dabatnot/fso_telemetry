[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 99)]
    [int]$PhaseNumber,

    [string]$PhaseDirectory,

    [string]$AnalysisDirectory = 'documentation/analysis',

    [switch]$AsJson
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)

function Read-StrictUtf8 {
    param([string]$Path)

    try {
        return [System.IO.File]::ReadAllText($Path, $script:utf8)
    }
    catch {
        throw "Invalid UTF-8: $Path"
    }
}

function Get-UniqueMatches {
    param(
        [string]$Text,
        [string]$Pattern
    )

    return @(
        [regex]::Matches($Text, $Pattern) |
            ForEach-Object { $_.Value } |
            Sort-Object -Unique
    )
}

if (-not (Test-Path -LiteralPath $AnalysisDirectory -PathType Container)) {
    throw "Analysis directory does not exist: $AnalysisDirectory"
}

$analysisRoot = (Resolve-Path -LiteralPath $AnalysisDirectory).Path
$specsRoot = Join-Path $analysisRoot 'specs'
if (-not (Test-Path -LiteralPath $specsRoot -PathType Container)) {
    throw "Specifications directory does not exist: $specsRoot"
}

if ([string]::IsNullOrWhiteSpace($PhaseDirectory)) {
    $candidates = @(
        Get-ChildItem -LiteralPath $specsRoot -Directory |
            Where-Object { $_.Name -match "^$PhaseNumber-" }
    )
    if ($candidates.Count -ne 1) {
        throw "Expected one specification directory for phase $PhaseNumber; found $($candidates.Count)."
    }
    $phaseRoot = $candidates[0].FullName
}
else {
    if (-not (Test-Path -LiteralPath $PhaseDirectory -PathType Container)) {
        throw "Phase directory does not exist: $PhaseDirectory"
    }
    $phaseRoot = (Resolve-Path -LiteralPath $PhaseDirectory).Path
}

$folderName = Split-Path -Leaf $phaseRoot
if ($folderName -notmatch "^$PhaseNumber-") {
    throw "Specification folder '$folderName' does not match phase $PhaseNumber."
}

$markdownFiles = @(Get-ChildItem -LiteralPath $phaseRoot -File -Filter '*.md' | Sort-Object Name)
if ($markdownFiles.Count -ne 8) {
    throw "Expected exactly 8 Markdown documents; found $($markdownFiles.Count)."
}

if (@($markdownFiles | Where-Object { $_.Name -ceq 'README.md' }).Count -ne 1) {
    throw 'Expected exactly one README.md.'
}

for ($index = 1; $index -le 7; $index++) {
    $prefix = '{0:D2}' -f $index
    $count = @($markdownFiles | Where-Object { $_.Name -match "^$prefix-.+\.md$" }).Count
    if ($count -ne 1) {
        throw "Expected exactly one '$prefix-*.md' document; found $count."
    }
}

$combinedBuilder = New-Object System.Text.StringBuilder
$documentInventory = @()

$requirementPattern = "(?<![A-Z0-9])P$PhaseNumber-[A-Z][A-Z0-9-]*-[0-9]{3}(?![0-9])"
$acceptancePattern = "(?<![A-Z0-9])P$PhaseNumber-AC-[0-9]{3}(?![0-9])"
$decisionPattern = "(?<![A-Z0-9])D$PhaseNumber-[0-9]{3}(?![0-9])"
$workPackagePattern = "(?<![A-Z0-9])P$PhaseNumber\.[0-9]+(?![0-9])"
$gatePattern = "(?<![A-Z0-9])G$PhaseNumber-[A-Z][A-Z0-9-]*(?![A-Z0-9-])"

foreach ($file in $markdownFiles) {
    $text = Read-StrictUtf8 $file.FullName
    [void]$combinedBuilder.AppendLine($text)

    $documentInventory += [pscustomobject]@{
        Name = $file.Name
        Lines = [System.IO.File]::ReadAllLines($file.FullName, $utf8).Count
        Requirements = @(
            Get-UniqueMatches $text $requirementPattern |
                Where-Object { $_ -notmatch "^P$PhaseNumber-AC-" }
        )
        AcceptanceCriteria = @(Get-UniqueMatches $text $acceptancePattern)
        Decisions = @(Get-UniqueMatches $text $decisionPattern)
        WorkPackages = @(Get-UniqueMatches $text $workPackagePattern)
        Gates = @(Get-UniqueMatches $text $gatePattern)
    }
}

$combinedText = $combinedBuilder.ToString()
$requirements = @(
    Get-UniqueMatches $combinedText $requirementPattern |
        Where-Object { $_ -notmatch "^P$PhaseNumber-AC-" }
)
$acceptanceCriteria = @(Get-UniqueMatches $combinedText $acceptancePattern)
$decisions = @(Get-UniqueMatches $combinedText $decisionPattern)
$workPackages = @(
    Get-UniqueMatches $combinedText $workPackagePattern |
        Sort-Object { [int](($_ -split '\.')[1]) }
)
$gates = @(Get-UniqueMatches $combinedText $gatePattern)

$checklistMatches = [regex]::Matches(
    $combinedText,
    '(?m)^[ \t]*-[ \t]+\[(?<state>[ xX])\][ \t]+(?<text>.+)$'
)
$checklist = @(
    foreach ($match in $checklistMatches) {
        [pscustomobject]@{
            Checked = $match.Groups['state'].Value -match '[xX]'
            Text = $match.Groups['text'].Value.Trim()
        }
    }
)

if ($requirements.Count -eq 0) {
    throw "No phase $PhaseNumber requirement IDs were found."
}
if ($acceptanceCriteria.Count -eq 0) {
    throw "No phase $PhaseNumber acceptance criteria were found."
}
if ($workPackages.Count -eq 0) {
    throw "No phase $PhaseNumber work packages were found."
}
if ($gates.Count -eq 0) {
    throw "No phase $PhaseNumber gates were found."
}

$result = [pscustomobject]@{
    PhaseNumber = $PhaseNumber
    PhaseDirectory = $phaseRoot
    Documents = $documentInventory
    RequirementIds = $requirements
    AcceptanceCriterionIds = $acceptanceCriteria
    DecisionIds = $decisions
    WorkPackageIds = $workPackages
    GateIds = $gates
    Checklist = $checklist
    Summary = [pscustomobject]@{
        DocumentCount = $markdownFiles.Count
        LineCount = ($documentInventory | Measure-Object -Property Lines -Sum).Sum
        RequirementCount = $requirements.Count
        AcceptanceCriterionCount = $acceptanceCriteria.Count
        DecisionCount = $decisions.Count
        WorkPackageCount = $workPackages.Count
        GateCount = $gates.Count
        CheckedChecklistItems = @($checklist | Where-Object { $_.Checked }).Count
        OpenChecklistItems = @($checklist | Where-Object { -not $_.Checked }).Count
    }
}

if ($AsJson) {
    $result | ConvertTo-Json -Depth 8
    exit 0
}

Write-Output "Phase $PhaseNumber contract inventory passed."
Write-Output "Directory: $phaseRoot"
Write-Output "Documents: $($result.Summary.DocumentCount)"
Write-Output "Lines: $($result.Summary.LineCount)"
Write-Output "Requirements: $($result.Summary.RequirementCount)"
Write-Output "Acceptance criteria: $($result.Summary.AcceptanceCriterionCount)"
Write-Output "Decisions: $($result.Summary.DecisionCount)"
Write-Output "Work packages: $($result.Summary.WorkPackageCount)"
Write-Output "Gates: $($result.Summary.GateCount)"
Write-Output "Checklist: $($result.Summary.CheckedChecklistItems) checked, $($result.Summary.OpenChecklistItems) open"
Write-Output "Requirement IDs: $($requirements -join ', ')"
Write-Output "Acceptance IDs: $($acceptanceCriteria -join ', ')"
Write-Output "Work packages: $($workPackages -join ', ')"
Write-Output "Gates: $($gates -join ', ')"
