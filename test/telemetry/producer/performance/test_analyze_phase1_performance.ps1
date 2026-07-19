[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$Analyzer = Join-Path $PSScriptRoot 'analyze_phase1_performance.ps1'
$Raw = Join-Path ([System.IO.Path]::GetTempPath()) ("fs2open-p93-" + [Guid]::NewGuid().ToString('N'))
$Report = Join-Path $Raw 'report.json'
$CallbackNames = @('baseline', 'config-absent', 'disabled')
$ActiveNames = @('active-one-client', 'active-four-clients', 'would-block-loss-resync')
$ActiveHeader = 'sample_index,tick_duration_ns,collect_ns,diff_ns,serialization_ns,network_ns,allocation_events,syscall_count,queue_depth,baselines_active,is_keyframe'

function Require([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}

function Write-CallbackCsv([string]$Name) {
	$Path = Join-Path $Raw "$Name.csv"
	$Writer = [System.IO.StreamWriter]::new($Path, $false, [System.Text.UTF8Encoding]::new($false))
	try {
		$Writer.WriteLine('sample_index,duration_ns')
		for ($Index = 0; $Index -lt 100000; ++$Index) {
			# Exactly 99,000 low samples means nearest-rank p99 (rank 99,000) is
			# 10 ns, not an interpolated value from the following high samples.
			$Duration = if ($Index -lt 99000) { 10 } else { 49000 }
			$Writer.WriteLine("$Index,$Duration")
		}
	} finally { $Writer.Dispose() }
}

function Write-ActiveCsv([string]$Name, [UInt64]$SteadyTickNs = 200000, [UInt64]$FinalAllocations = 7, [UInt64]$Baselines = 1) {
	$Path = Join-Path $Raw "$Name.csv"
	$Writer = [System.IO.StreamWriter]::new($Path, $false, [System.Text.UTF8Encoding]::new($false))
	try {
		$Writer.WriteLine($ActiveHeader)
		# Deliberately large keyframe: it must be reported, but excluded from p99.
		$Writer.WriteLine("0,1000000,100000,100000,100000,100000,7,1,1,$Baselines,1")
		for ($Index = 1; $Index -le 100; ++$Index) {
			$Allocation = if ($Index -eq 100) { $FinalAllocations } else { 7 }
			$Writer.WriteLine("$Index,$SteadyTickNs,50000,50000,50000,50000,$Allocation,1,1,$Baselines,0")
		}
	} finally { $Writer.Dispose() }
}

function Invoke-Analyzer([string]$Output) {
	& $Analyzer -RawDirectory $Raw -OutputReport $Output -Revision 'test-revision' -BuildType Release `
		-Compiler 'MSVC test' -Platform 'Win32' -Cpu 'test-cpu' -PowerMode 'test-power' `
		-Mission 'test-mission' -FlightHz 30 -StateBytes 512 -Duration '60s' -Warmup '10s' -Command 'test-command' | Out-Null
}

try {
	New-Item -ItemType Directory -Path $Raw | Out-Null
	foreach ($Name in $CallbackNames) { Write-CallbackCsv $Name }
	Write-ActiveCsv 'active-one-client'
	Write-ActiveCsv 'active-four-clients' 200000 7 4
	Write-ActiveCsv 'would-block-loss-resync'

	Invoke-Analyzer $Report
	Require (Test-Path -LiteralPath $Report -PathType Leaf) 'Analyzer did not emit the reproducibility report.'
	$Result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
	Require ($Result.schema -eq 'fs2open.telemetry.phase1.performance.v1') 'Unexpected report schema.'
	Require ($Result.requirement -eq 'P1-REQ-033' -and $Result.decision -eq 'D1-014') 'Missing contract IDs.'
	Require ($Result.callback_workloads.Count -eq 3 -and $Result.active_workloads.Count -eq 3) 'Report must retain all six workloads.'
	Require ($Result.callback_workloads[2].samples -eq 100000) 'Disabled workload sample count changed.'
	Require ([Math]::Abs($Result.callback_workloads[2].p99_ms - 0.00001) -lt 0.000000001) 'p99 is not nearest-rank over raw samples.'
	Require ($Result.active_workloads[0].keyframe_samples -eq 1 -and $Result.active_workloads[0].steady_samples -eq 100) 'Keyframe separation changed.'
	Require ([Math]::Abs($Result.active_workloads[0].steady_p99_ms - 0.2) -lt 0.000000001) 'Keyframe was included in steady p99.'
	Require ($Result.active_workloads[0].allocation_events_steady_delta -eq 0) 'Steady allocation delta was not preserved.'
	Require ($Result.active_workloads[1].maximum_baselines_active -eq 4 -and $Result.active_workloads[1].baseline_limit -eq 4) 'Four-client baseline limit was not workload-aware.'
	foreach ($Field in @('revision', 'build_type', 'compiler', 'platform', 'cpu', 'power_mode', 'mission', 'flight_hz', 'state_bytes', 'duration', 'warmup', 'command')) {
		Require ($null -ne $Result.metadata.$Field -and [string]$Result.metadata.$Field -ne '') "Missing reproducibility metadata: $Field"
	}

	Write-ActiveCsv 'active-one-client' 250001 7
	$Rejected = $false
	try { Invoke-Analyzer (Join-Path $Raw 'threshold-fail.json') } catch { $Rejected = $true }
	Require $Rejected 'Analyzer accepted an active steady p99 above 0.25 ms.'

	Write-ActiveCsv 'active-one-client' 200000 8
	$Rejected = $false
	try { Invoke-Analyzer (Join-Path $Raw 'allocation-fail.json') } catch { $Rejected = $true }
	Require $Rejected 'Analyzer accepted a steady allocation increase.'

	Write-Output 'P9.3 analyzer contract PASS'
} finally {
	if (Test-Path -LiteralPath $Raw) { Remove-Item -LiteralPath $Raw -Recurse -Force }
}
