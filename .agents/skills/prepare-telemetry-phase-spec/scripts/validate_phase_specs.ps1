param(
    [Parameter(Mandatory = $true)][int]$PhaseNumber,
    [Parameter(Mandatory = $true)][string]$PhaseDirectory
)

$ErrorActionPreference = 'Stop'
$errors = [System.Collections.Generic.List[string]]::new()

if (-not (Test-Path -LiteralPath $PhaseDirectory -PathType Container)) {
    throw "Phase directory not found: $PhaseDirectory"
}

$docs = @(Get-ChildItem -LiteralPath $PhaseDirectory -File -Filter '*.md')
$expected = @('README.md') + (1..7 | ForEach-Object { '{0:d2}' -f $_ })

if ($docs.Count -ne 8) {
    $errors.Add("Expected exactly 8 Markdown documents, found $($docs.Count).")
}
if (-not ($docs.Name -contains 'README.md')) {
    $errors.Add('README.md is missing.')
}
foreach ($prefix in 1..7) {
    $matches = @($docs | Where-Object Name -Match ('^{0:d2}-' -f $prefix))
    if ($matches.Count -ne 1) {
        $errors.Add("Expected exactly one document with prefix $('{0:d2}' -f $prefix), found $($matches.Count).")
    }
}

$doc06 = @($docs | Where-Object Name -Match '^06-') | Select-Object -First 1
$doc07 = @($docs | Where-Object Name -Match '^07-') | Select-Object -First 1
if ($null -eq $doc06 -or $null -eq $doc07) {
    $errors.Add('Documents 06 and 07 are required for evidence validation.')
} else {
    $validation = Get-Content -LiteralPath $doc06.FullName -Raw -Encoding UTF8
    $delivery = Get-Content -LiteralPath $doc07.FullName -Raw -Encoding UTF8
    $budgetMatch = [regex]::Match($validation, '<!--\s*certification-budget-minutes:\s*(\d+)\s*-->')
    if (-not $budgetMatch.Success) {
        $errors.Add('Document 06 must contain certification-budget-minutes.')
    } else {
        $budget = [int]$budgetMatch.Groups[1].Value
        if ($budget -gt 180 -and $validation -notmatch '(?i)Certification budget exception') {
            $errors.Add("Certification budget is $budget minutes without a documented exception.")
        }
    }

    foreach ($term in @('Risque couvert','Dur.e estim.e','Invalidation','R.utilisation','Crit.re d.arr.t')) {
        if ($validation -notmatch $term) {
            $errors.Add("Document 06 is missing evidence metadata matching: $term.")
        }
    }
    foreach ($term in @('inner-loop','readiness','wp-checkpoint','gate-certification','tracker')) {
        if ($delivery -notmatch [regex]::Escape($term)) {
            $errors.Add("Document 07 is missing execution metadata: $term.")
        }
    }

    $durationMatches = [regex]::Matches($validation, '(?m)^\|[^\r\n]*\|\s*(\d+)\s+min\s*\|')
    $durationSum = 0
    foreach ($match in $durationMatches) { $durationSum += [int]$match.Groups[1].Value }
    if ($durationMatches.Count -eq 0) {
        $errors.Add('Document 06 has no machine-summable evidence duration rows.')
    } elseif ($budgetMatch.Success -and $durationSum -gt [int]$budgetMatch.Groups[1].Value) {
        $errors.Add("Evidence durations total $durationSum minutes and exceed the declared budget.")
    }

    $evidenceRows = @($validation -split "`r?`n" | Where-Object { $_ -match '^\|\s*[^-|].*\|\s*`(?:wp-checkpoint|gate-certification)`\s*\|' })
    foreach ($row in $evidenceRows) {
        if (($row.ToCharArray() | Where-Object { $_ -eq '|' }).Count -lt 9) {
            $errors.Add("Incomplete expensive-evidence row: $row")
        }
    }
}

foreach ($doc in $docs) {
    try {
        $bytes = [System.IO.File]::ReadAllBytes($doc.FullName)
        $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
        $content = $strictUtf8.GetString($bytes)
    } catch {
        $errors.Add("$($doc.Name) is not strict UTF-8.")
        continue
    }
    if ($content -match '(?im)\b(TODO|TBD)\b') {
        $errors.Add("$($doc.Name) contains TODO or TBD.")
    }
    if ($content -match '(?i)phase\s+(?!' + [regex]::Escape([string]$PhaseNumber) + '\b)\d+\s+est\s+termin') {
        $errors.Add("$($doc.Name) appears to claim another phase is complete.")
    }
    if (([regex]::Matches($content, '(?m)^```')).Count % 2 -ne 0) {
        $errors.Add("$($doc.Name) has unbalanced fenced code blocks.")
    }
    if ($content -match '(?m)[ \t]+$') {
        $errors.Add("$($doc.Name) contains trailing whitespace.")
    }
    foreach ($link in [regex]::Matches($content, '\[[^\]]+\]\(([^)#]+)(?:#[^)]+)?\)')) {
        $target = $link.Groups[1].Value
        if ($target -match '^(?:https?://|mailto:)') { continue }
        $resolved = Join-Path $doc.DirectoryName ([uri]::UnescapeDataString($target))
        if (-not (Test-Path -LiteralPath $resolved)) {
            $errors.Add("$($doc.Name) has a missing local link target: $target")
        }
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { [Console]::Error.WriteLine("ERROR $_") }
    exit 1
}

Write-Output "PASS phase=$PhaseNumber documents=8 certificationBudgetMinutes=$budget evidenceDurationMinutes=$durationSum"
