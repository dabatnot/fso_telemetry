[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 99)]
    [int]$PhaseNumber,

    [Parameter(Mandatory = $true)]
    [string]$PhaseDirectory,

    [string]$AnalysisDirectory = 'documentation/analysis',

    [string]$RoadmapPath = 'documentation/analysis/04-implementation-roadmap.md'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$errors = New-Object 'System.Collections.Generic.List[string]'
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)

function Add-ValidationError {
    param([string]$Message)
    $script:errors.Add($Message)
}

function Read-StrictUtf8 {
    param([string]$Path)

    try {
        return [System.IO.File]::ReadAllText($Path, $script:utf8)
    }
    catch {
        Add-ValidationError "Invalid UTF-8: $Path"
        return $null
    }
}

function ConvertTo-MarkdownSlug {
    param([string]$Heading)

    $visible = [regex]::Replace($Heading, '!\[([^\]]*)\]\([^)]+\)', '$1')
    $visible = [regex]::Replace($visible, '\[([^\]]+)\]\([^)]+\)', '$1')
    $visible = [regex]::Replace($visible, '<[^>]+>', '')
    $visible = $visible -replace '[`*_~]', ''
    $visible = $visible.Trim().ToLowerInvariant()

    $builder = New-Object System.Text.StringBuilder
    foreach ($character in $visible.ToCharArray()) {
        $category = [System.Globalization.CharUnicodeInfo]::GetUnicodeCategory($character)
        if ([char]::IsLetterOrDigit($character) -or
            $category -eq [System.Globalization.UnicodeCategory]::NonSpacingMark -or
            $character -eq '-' -or $character -eq '_') {
            [void]$builder.Append($character)
        }
        elseif ([char]::IsWhiteSpace($character)) {
            [void]$builder.Append('-')
        }
    }

    return $builder.ToString()
}

function ConvertTo-SearchForm {
    param([string]$Text)

    $normalized = $Text.Normalize([System.Text.NormalizationForm]::FormD)
    $builder = New-Object System.Text.StringBuilder
    foreach ($character in $normalized.ToCharArray()) {
        $category = [System.Globalization.CharUnicodeInfo]::GetUnicodeCategory($character)
        if ($category -ne [System.Globalization.UnicodeCategory]::NonSpacingMark) {
            [void]$builder.Append($character)
        }
    }
    return $builder.ToString()
}

function Get-MarkdownAnchors {
    param([string]$Text)

    $anchors = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $occurrences = @{}
    $matches = [regex]::Matches($Text, '(?m)^#{1,6}[ \t]+(?<heading>.+?)[ \t]*$')

    foreach ($match in $matches) {
        $heading = $match.Groups['heading'].Value -replace '[ \t]+#+[ \t]*$', ''
        $baseSlug = ConvertTo-MarkdownSlug $heading
        if ([string]::IsNullOrWhiteSpace($baseSlug)) {
            continue
        }

        if ($occurrences.ContainsKey($baseSlug)) {
            $occurrences[$baseSlug] = [int]$occurrences[$baseSlug] + 1
            $slug = "$baseSlug-$($occurrences[$baseSlug])"
        }
        else {
            $occurrences[$baseSlug] = 0
            $slug = $baseSlug
        }
        [void]$anchors.Add($slug)
    }

    return $anchors
}

function Remove-CodeForLinkScan {
    param([string]$Text)

    $result = [regex]::Replace($Text, '(?ms)^```.*?^```[ \t]*$', '')
    $result = [regex]::Replace($result, '(?ms)^~~~.*?^~~~[ \t]*$', '')
    return [regex]::Replace($result, '`[^`\r\n]*`', '')
}

if (-not (Test-Path -LiteralPath $PhaseDirectory -PathType Container)) {
    throw "Phase directory does not exist: $PhaseDirectory"
}
if (-not (Test-Path -LiteralPath $AnalysisDirectory -PathType Container)) {
    throw "Analysis directory does not exist: $AnalysisDirectory"
}
if (-not (Test-Path -LiteralPath $RoadmapPath -PathType Leaf)) {
    throw "Roadmap does not exist: $RoadmapPath"
}

$phaseRoot = (Resolve-Path -LiteralPath $PhaseDirectory).Path
$analysisRoot = (Resolve-Path -LiteralPath $AnalysisDirectory).Path
$roadmapFullPath = (Resolve-Path -LiteralPath $RoadmapPath).Path
$folderName = Split-Path -Leaf $phaseRoot

if ($folderName -notmatch "^$PhaseNumber-") {
    Add-ValidationError "Folder '$folderName' does not start with '$PhaseNumber-'."
}

$markdownFiles = @(Get-ChildItem -LiteralPath $phaseRoot -File -Filter '*.md' | Sort-Object Name)
if ($markdownFiles.Count -ne 8) {
    Add-ValidationError "Expected exactly 8 Markdown files; found $($markdownFiles.Count)."
}

$readmes = @($markdownFiles | Where-Object { $_.Name -ceq 'README.md' })
if ($readmes.Count -ne 1) {
    Add-ValidationError "Expected exactly one README.md; found $($readmes.Count)."
}

for ($index = 1; $index -le 7; $index++) {
    $prefix = '{0:D2}' -f $index
    $matches = @($markdownFiles | Where-Object { $_.Name -match "^$prefix-.+\.md$" })
    if ($matches.Count -ne 1) {
        Add-ValidationError "Expected exactly one '$prefix-*.md' document; found $($matches.Count)."
    }
}

$textsByPath = @{}
$combinedTextBuilder = New-Object System.Text.StringBuilder

foreach ($file in $markdownFiles) {
    $text = Read-StrictUtf8 $file.FullName
    if ($null -eq $text) {
        continue
    }
    $textsByPath[$file.FullName] = $text
    [void]$combinedTextBuilder.AppendLine($text)
    $searchText = ConvertTo-SearchForm $text

    if ($text.IndexOf([char]0) -ge 0 -or $text.IndexOf([char]0xfffd) -ge 0) {
        Add-ValidationError "$($file.Name): contains NUL or U+FFFD."
    }
    if ($text.Length -lt 800) {
        Add-ValidationError "$($file.Name): content is too short for an exhaustive phase document."
    }
    if ($text -notmatch '(?m)^# [^#\r\n]+') {
        Add-ValidationError "$($file.Name): missing a level-1 title."
    }
    if ($text -match '(?m)[ \t]+$') {
        Add-ValidationError "$($file.Name): contains trailing whitespace."
    }
    if ($searchText -match '(?im)\b(TODO|TBD|FIXME|XXX)\b|a definir|reste a definir|\[PLACEHOLDER\]') {
        Add-ValidationError "$($file.Name): contains an unresolved marker."
    }

    $backtickFences = [regex]::Matches($text, '(?m)^[ \t]*```').Count
    $tildeFences = [regex]::Matches($text, '(?m)^[ \t]*~~~').Count
    if (($backtickFences % 2) -ne 0) {
        Add-ValidationError "$($file.Name): unbalanced backtick fences."
    }
    if (($tildeFences % 2) -ne 0) {
        Add-ValidationError "$($file.Name): unbalanced tilde fences."
    }
}

foreach ($file in $markdownFiles) {
    if (-not $textsByPath.ContainsKey($file.FullName)) {
        continue
    }

    $scanText = Remove-CodeForLinkScan $textsByPath[$file.FullName]
    $linkMatches = [regex]::Matches($scanText, '(?<!!)\[[^\]]*\]\((?<target>[^)]+)\)')

    foreach ($linkMatch in $linkMatches) {
        $rawTarget = $linkMatch.Groups['target'].Value.Trim().Trim('<', '>')
        if ($rawTarget -match '^[A-Za-z][A-Za-z0-9+.-]*:') {
            continue
        }

        $parts = $rawTarget -split '#', 2
        $relativePath = [uri]::UnescapeDataString($parts[0])
        $fragment = if ($parts.Count -eq 2) { [uri]::UnescapeDataString($parts[1]) } else { '' }

        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            $targetPath = $file.FullName
        }
        elseif ([System.IO.Path]::IsPathRooted($relativePath)) {
            $targetPath = [System.IO.Path]::GetFullPath($relativePath)
        }
        else {
            $targetPath = [System.IO.Path]::GetFullPath((Join-Path $file.DirectoryName $relativePath))
        }

        if (-not (Test-Path -LiteralPath $targetPath)) {
            Add-ValidationError "$($file.Name): missing local link target '$rawTarget'."
            continue
        }

        if (-not [string]::IsNullOrWhiteSpace($fragment) -and
            (Test-Path -LiteralPath $targetPath -PathType Leaf) -and
            [System.IO.Path]::GetExtension($targetPath) -ieq '.md') {
            $targetText = Read-StrictUtf8 $targetPath
            if ($null -eq $targetText) {
                continue
            }
            $anchors = Get-MarkdownAnchors $targetText
            if (-not $anchors.Contains($fragment)) {
                Add-ValidationError "$($file.Name): missing anchor '#$fragment' in '$targetPath'."
            }
        }
    }
}

$combinedText = $combinedTextBuilder.ToString()
$analysisSources = @(Get-ChildItem -LiteralPath $analysisRoot -File -Filter '*.md' | Sort-Object Name)
foreach ($source in $analysisSources) {
    if ($combinedText.IndexOf($source.Name, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        Add-ValidationError "Traceability is missing top-level analysis source '$($source.Name)'."
    }
}

if ($combinedText.IndexOf((Split-Path -Leaf $roadmapFullPath), [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
    Add-ValidationError "The roadmap is not referenced by filename."
}

if ($readmes.Count -eq 1 -and $textsByPath.ContainsKey($readmes[0].FullName)) {
    $readmeText = $textsByPath[$readmes[0].FullName]
    $readmeSearch = ConvertTo-SearchForm $readmeText
    foreach ($pattern in @('(?im)^## .*Statut', '(?im)^## .*Documents', '(?im)^## .*Sources', '(?im)^## .*(Gate|Critere.*sortie)')) {
        if ($readmeSearch -notmatch $pattern) {
            Add-ValidationError "README.md: missing required section matching '$pattern'."
        }
    }
}

$doc01 = @($markdownFiles | Where-Object { $_.Name -match '^01-.+\.md$' })
if ($doc01.Count -eq 1 -and $textsByPath.ContainsKey($doc01[0].FullName)) {
    $doc01Search = ConvertTo-SearchForm $textsByPath[$doc01[0].FullName]
    if ($doc01Search -notmatch '(?i)(Perimetre|Portee)') {
        Add-ValidationError "$($doc01[0].Name): missing scope section."
    }
}

$doc06 = @($markdownFiles | Where-Object { $_.Name -match '^06-.+\.md$' })
if ($doc06.Count -eq 1 -and $textsByPath.ContainsKey($doc06[0].FullName)) {
    $doc06Search = ConvertTo-SearchForm $textsByPath[$doc06[0].FullName]
    if ($doc06Search -notmatch '(?i)Tests?' -or $doc06Search -notmatch '(?i)(Securite|Validation)') {
        Add-ValidationError "$($doc06[0].Name): must cover tests and validation/security."
    }
}

$doc07 = @($markdownFiles | Where-Object { $_.Name -match '^07-.+\.md$' })
if ($doc07.Count -eq 1 -and $textsByPath.ContainsKey($doc07[0].FullName)) {
    $doc07Text = $textsByPath[$doc07[0].FullName]
    $doc07Search = ConvertTo-SearchForm $doc07Text
    foreach ($term in @('Criteres d.acceptation', 'Matrice de tracabilite', 'Risques', 'Checklist')) {
        if ($doc07Search -notmatch "(?i)$term") {
            Add-ValidationError "$($doc07[0].Name): missing '$term'."
        }
    }
    if ($doc07Text -match '(?m)^[ \t]*-[ \t]+\[[xX]\]') {
        Add-ValidationError "$($doc07[0].Name): completion checklist contains checked items before evidence exists."
    }
}

if ($errors.Count -gt 0) {
    [Console]::Error.WriteLine("Phase specification validation failed with $($errors.Count) error(s):")
    foreach ($validationError in $errors) {
        [Console]::Error.WriteLine("- $validationError")
    }
    exit 1
}

$lineCount = 0
foreach ($file in $markdownFiles) {
    if ($textsByPath.ContainsKey($file.FullName)) {
        $lineCount += [System.IO.File]::ReadAllLines($file.FullName, $utf8).Count
    }
}

Write-Output "Phase $PhaseNumber specification validation passed."
Write-Output "Directory: $phaseRoot"
Write-Output "Documents: $($markdownFiles.Count)"
Write-Output "Lines: $lineCount"
Write-Output "Analysis sources traced: $($analysisSources.Count)"
Write-Output 'Checks: structure, UTF-8, headings, fences, links, anchors, placeholders, traceability, required sections.'
