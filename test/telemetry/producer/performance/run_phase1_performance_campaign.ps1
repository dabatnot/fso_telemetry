[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$DisabledBenchmark,
    [Parameter(Mandatory = $true)] [string]$NativeRunner,
	[Parameter(Mandatory = $true)] [string]$Compiler,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'raw'),
    [string]$ReportPath = (Join-Path $PSScriptRoot 'reports\phase1-performance.json'),
    [ValidateSet('Debug', 'Release')] [string]$BuildType = 'Release',
    [int]$FlightHz = 30,
    [int]$StateBytes = 0,
    [int]$ActiveSamples = 100000,
    [int]$WarmupSamples = 10000,
    [UInt64]$Seed = 424242,
    [string]$Mission = 'native integration harness',
    [switch]$Resume
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ($FlightHz -lt 1 -or $ActiveSamples -lt 1 -or $WarmupSamples -lt 0) { throw 'Invalid flight or sample count.' }
foreach ($Path in @($DisabledBenchmark, $NativeRunner)) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Benchmark executable is unavailable: $Path" }
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$Analyzer = Join-Path $PSScriptRoot 'analyze_phase1_performance.ps1'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$ReportParent = Split-Path -Parent $ReportPath
if ($ReportParent) { New-Item -ItemType Directory -Force -Path $ReportParent | Out-Null }
$LogDirectory = Join-Path $OutputDirectory 'campaign-command-logs'
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

$Started = [DateTime]::UtcNow
$Revision = (git -C $Root rev-parse HEAD).Trim()
if ([string]::IsNullOrWhiteSpace($Revision)) { throw 'Cannot determine the measured revision.' }
$Computer = Get-CimInstance Win32_ComputerSystem
$Processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$Os = Get-CimInstance Win32_OperatingSystem
$PowerMode = (powercfg /getactivescheme 2>$null | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($PowerMode)) { $PowerMode = 'unavailable' }
$ManifestPath = Join-Path $OutputDirectory 'campaign-manifest.json'
$Manifest = [ordered]@{
    schema = 'fs2open.telemetry.phase1.performance-campaign.v2'; requirement = 'P1-REQ-033'; decision = 'D1-014'
    status = 'running'; started_utc = $Started.ToString('o'); completed_utc = $null; failure_reason = $null; revision = $Revision
    build_type = $BuildType; compiler = $Compiler; os = "$($Os.Caption) $($Os.Version)"; platform = $env:PROCESSOR_ARCHITECTURE
    cpu = $Processor.Name; logical_processors = $Computer.NumberOfLogicalProcessors; power_mode = $PowerMode
    mission = $Mission; flight_hz = $FlightHz; state_bytes = $StateBytes; active_samples = $ActiveSamples; warmup_samples = $WarmupSamples; seed = $Seed
    resume = [bool]$Resume; disabled_benchmark = (Resolve-Path -LiteralPath $DisabledBenchmark).Path; native_runner = (Resolve-Path -LiteralPath $NativeRunner).Path
    commands = [System.Collections.ArrayList]::new()
}

function Save-Manifest {
    # Persist each transition so an interrupted campaign explains exactly where
    # it stopped; raw CSVs are never created or rewritten by this function.
    $Manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ManifestPath -Encoding utf8
}

function Test-ExistingCsv([string]$Path, [string[]]$AcceptedHeaders, [int]$ExpectedRows) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try {
        $Lines = @(Get-Content -LiteralPath $Path -ErrorAction Stop)
		$Header = $Lines[0].Split(',')
		if ($Lines.Count -ne ($ExpectedRows + 1) -or -not ($AcceptedHeaders -contains $Lines[0])) { return $false }
        for ($Index = 1; $Index -lt $Lines.Count; ++$Index) {
            $Fields = $Lines[$Index].Split(',')
            if ($Fields.Count -ne $Header.Count -or $Fields[0] -ne [string]($Index - 1)) { return $false }
        }
        return $true
    } catch { return $false }
}

function Add-CommandRecord([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$OutputPath) {
    $Record = [ordered]@{
        name = $Name; executable = $Executable; arguments = @($Arguments); output_csv = $OutputPath
        status = 'pending'; started_utc = $null; completed_utc = $null; exit_code = $null; fail_reason = $null
        stdout = $null; stderr = $null
    }
    [void]$Manifest.commands.Add($Record)
    Save-Manifest
    return $Record
}

function Invoke-CampaignCommand($Record) {
    $SafeName = ($Record.name -replace '[^A-Za-z0-9_.-]', '_')
    $Record.stdout = Join-Path $LogDirectory "$SafeName.stdout.log"
    $Record.stderr = Join-Path $LogDirectory "$SafeName.stderr.log"
    $Record.status = 'running'; $Record.started_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
    try {
        # The call operator preserves each array element as one native argument.
        # Start-Process flattens ArgumentList on Windows PowerShell 5.1, which
        # breaks the analyzer's space-containing -Command metadata and can bind
        # its named parameters a second time.
        & $Record.executable @($Record.arguments) 1> $Record.stdout 2> $Record.stderr
        $Record.exit_code = $LASTEXITCODE
        if ($Record.exit_code -ne 0) { throw "exit code $($Record.exit_code)" }
        $Record.status = 'completed'
    } catch {
        $Record.status = 'failed'; $Record.fail_reason = $_.Exception.Message
        throw "Campaign command '$($Record.name)' failed: $($Record.fail_reason)"
    } finally {
        $Record.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
    }
}

$CallbackHeader = @('sample_index', 'duration_ns')
$ActiveLegacyHeader = @('sample_index', 'tick_duration_ns', 'collect_ns', 'diff_ns', 'serialization_ns', 'network_ns', 'allocation_events', 'syscall_count', 'queue_depth', 'baselines_active', 'is_keyframe')
$ActiveHeader = @('sample_index', 'tick_duration_ns', 'collect_ns', 'diff_ns', 'state_image_build_ns', 'state_image_fill_ns', 'state_image_publish_validate_ns', 'state_image_adopt_ns', 'state_image_semantic_validate_ns', 'delta_build_ns', 'serialization_ns', 'network_ns', 'allocation_events', 'syscall_count', 'queue_depth', 'baselines_active', 'is_keyframe')
$CallbackAcceptedHeaders = @(($CallbackHeader -join ','))
$ActiveAcceptedHeaders = @(($ActiveLegacyHeader -join ','), ($ActiveHeader -join ','))
$Jobs = @(
	[pscustomobject]@{ name = 'baseline'; executable = $DisabledBenchmark; arguments = @('baseline'); rows = 100000; headers = $CallbackAcceptedHeaders },
	[pscustomobject]@{ name = 'config-absent'; executable = $DisabledBenchmark; arguments = @('config-absent'); rows = 100000; headers = $CallbackAcceptedHeaders },
	[pscustomobject]@{ name = 'disabled'; executable = $DisabledBenchmark; arguments = @('enabled-false'); rows = 100000; headers = $CallbackAcceptedHeaders },
	[pscustomobject]@{ name = 'active-one-client'; executable = $NativeRunner; arguments = @('--workload', 'active-one-client'); rows = $ActiveSamples; headers = $ActiveAcceptedHeaders },
	[pscustomobject]@{ name = 'active-four-clients'; executable = $NativeRunner; arguments = @('--workload', 'active-four-clients'); rows = $ActiveSamples; headers = $ActiveAcceptedHeaders },
	[pscustomobject]@{ name = 'would-block-loss-resync'; executable = $NativeRunner; arguments = @('--workload', 'would-block-loss-resync'); rows = $ActiveSamples; headers = $ActiveAcceptedHeaders }
)

try {
    Save-Manifest # Flush before the first workload, including an abrupt Ctrl+C.
    foreach ($Job in $Jobs) {
        $CsvPath = Join-Path $OutputDirectory "$($Job.name).csv"
        $Arguments = @($Job.arguments)
        if ($Job.executable -eq $NativeRunner) {
            $Arguments += @('--output', $CsvPath, '--samples', "$ActiveSamples", '--warmup', "$WarmupSamples", '--flight-hz', "$FlightHz", '--seed', "$Seed")
        } else {
            # The disabled benchmark has a positional output path contract.
            $Arguments += @($CsvPath)
        }
        $Record = Add-CommandRecord $Job.name $Job.executable $Arguments $CsvPath
        if (Test-Path -LiteralPath $CsvPath -PathType Leaf) {
            if (-not $Resume) {
                $Record.status = 'failed'; $Record.fail_reason = "Raw artifact already exists (use -Resume only for a valid CSV): $CsvPath"
                $Record.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest; throw $Record.fail_reason
            }
			if (-not (Test-ExistingCsv $CsvPath $Job.headers $Job.rows)) {
                $Record.status = 'failed'; $Record.fail_reason = "Existing raw artifact is invalid and will not be overwritten: $CsvPath"
                $Record.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest; throw $Record.fail_reason
            }
            $Record.status = 'skipped-valid-existing'; $Record.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
            continue
        }
        Invoke-CampaignCommand $Record
		if (-not (Test-ExistingCsv $CsvPath $Job.headers $Job.rows)) {
            $Record.status = 'failed'; $Record.fail_reason = "Runner completed but wrote an invalid CSV: $CsvPath"
            $Record.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest; throw $Record.fail_reason
        }
    }
    $CampaignCommand = (($Manifest.commands | ForEach-Object { "$($_.executable) $($_.arguments -join ' ')" }) -join ' ; ')
    $AnalyzerArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Analyzer,
        '-RawDirectory', $OutputDirectory, '-OutputReport', $ReportPath, '-Revision', $Revision, '-BuildType', $BuildType,
        '-Compiler', $Compiler, '-Platform', $Manifest.platform, '-Cpu', $Manifest.cpu, '-PowerMode', $Manifest.power_mode,
        '-Mission', $Mission, '-FlightHz', "$FlightHz", '-StateBytes', "$StateBytes",
        '-Duration', (([DateTime]::UtcNow - $Started).ToString()), '-Warmup', "$WarmupSamples samples", '-Command', $CampaignCommand)
    $AnalyzerRecord = Add-CommandRecord 'analyzer' (Get-Process -Id $PID).Path $AnalyzerArguments $ReportPath
    Invoke-CampaignCommand $AnalyzerRecord
    $Manifest.status = 'completed'
} catch {
    $Manifest.status = 'failed'; $Manifest.failure_reason = $_.Exception.Message
    throw
} finally {
    $Manifest.completed_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
}
