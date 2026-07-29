param([Parameter(Mandatory = $true)][int]$PhaseNumber)

$dirs = @(Get-ChildItem -LiteralPath 'documentation/analysis/specs' -Directory | Where-Object Name -Like "$PhaseNumber-*")
if ($dirs.Count -ne 1) { throw "Expected one phase directory for $PhaseNumber, found $($dirs.Count)" }
$docs = @(Get-ChildItem -LiteralPath $dirs[0].FullName -File -Filter '*.md')
if ($docs.Count -ne 8) { throw "Expected 8 Markdown documents, found $($docs.Count)" }

$all = $docs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 }
$ids = [regex]::Matches(($all -join "`n"), "\b(?:P$PhaseNumber-(?:REQ|AC|WP|TST)-\d+|G$PhaseNumber-[A-Z]+|D$PhaseNumber-\d+)\b") | ForEach-Object Value | Sort-Object -Unique

[pscustomobject]@{
    PhaseDirectory = $dirs[0].FullName
    Documents = $docs.Count
    Requirements = @($ids | Where-Object { $_ -like "P$PhaseNumber-REQ-*" }).Count
    AcceptanceCriteria = @($ids | Where-Object { $_ -like "P$PhaseNumber-AC-*" }).Count
    WorkPackages = @($ids | Where-Object { $_ -like "P$PhaseNumber-WP-*" }).Count
    Tests = @($ids | Where-Object { $_ -like "P$PhaseNumber-TST-*" }).Count
    Gates = @($ids | Where-Object { $_ -like "G$PhaseNumber-*" }).Count
    Decisions = @($ids | Where-Object { $_ -like "D$PhaseNumber-*" }).Count
} | Format-List
