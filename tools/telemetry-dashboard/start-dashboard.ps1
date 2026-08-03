[CmdletBinding()]
param(
    [string]$TelemetryHost = "127.0.0.1",
    [ValidateRange(1, 65535)]
    [int]$TelemetryPort = 42042,
    [ValidateRange(1, 65535)]
    [int]$UiPort = 43100,
    [string]$TelemetryConfig,
    [string]$Replay,
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"
$DashboardRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $DashboardRoot "..\..")).Path
$BackendRoot = Join-Path $DashboardRoot "backend"
$FrontendRoot = Join-Path $DashboardRoot "frontend"
$BuildRoot = Join-Path $RepoRoot "build\telemetry-dashboard"
$VirtualEnv = Join-Path $BuildRoot ".venv"
$PythonExe = Join-Path $VirtualEnv "Scripts\python.exe"
$Requirements = Join-Path $BackendRoot "requirements.lock.txt"
$RequirementStamp = Join-Path $VirtualEnv ".requirements.sha256"

$FlightHz = 30
$SystemsHz = 10
$MissionHeartbeatMs = 500

if ($TelemetryConfig) {
    $ResolvedConfig = (Resolve-Path -LiteralPath $TelemetryConfig).Path
    $Config = Get-Content -LiteralPath $ResolvedConfig -Raw | ConvertFrom-Json
    if ($null -ne $Config.bindPort -and -not $PSBoundParameters.ContainsKey("TelemetryPort")) {
        $TelemetryPort = [int]$Config.bindPort
    }
    if ($null -ne $Config.flightHz) { $FlightHz = [int]$Config.flightHz }
    if ($null -ne $Config.systemsHz) { $SystemsHz = [int]$Config.systemsHz }
    if ($null -ne $Config.missionHeartbeatMs) { $MissionHeartbeatMs = [int]$Config.missionHeartbeatMs }
}

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath $PythonExe)) {
    python -m venv $VirtualEnv
}

$CurrentRequirementsHash = (Get-FileHash -LiteralPath $Requirements -Algorithm SHA256).Hash
$InstalledRequirementsHash = if (Test-Path -LiteralPath $RequirementStamp) {
    (Get-Content -LiteralPath $RequirementStamp -Raw).Trim()
} else {
    ""
}
if ($CurrentRequirementsHash -ne $InstalledRequirementsHash) {
    & $PythonExe -m pip install --disable-pip-version-check -r $Requirements
    Set-Content -LiteralPath $RequirementStamp -Value $CurrentRequirementsHash -Encoding ascii
}

Push-Location $FrontendRoot
try {
    if (-not (Test-Path -LiteralPath (Join-Path $FrontendRoot "node_modules"))) {
        npm ci
    }
    npm run build
}
finally {
    Pop-Location
}

$Arguments = @(
    (Join-Path $BackendRoot "app.py"),
    "--telemetry-host", $TelemetryHost,
    "--telemetry-port", "$TelemetryPort",
    "--ui-port", "$UiPort",
    "--flight-hz", "$FlightHz",
    "--systems-hz", "$SystemsHz",
    "--mission-heartbeat-ms", "$MissionHeartbeatMs"
)
if ($Replay) {
    $Arguments += @("--replay", (Resolve-Path -LiteralPath $Replay).Path)
}

if (-not $NoBrowser) {
    $Url = "http://127.0.0.1:$UiPort"
    Start-Job -ScriptBlock {
        param($DashboardUrl)
        Start-Sleep -Milliseconds 1200
        Start-Process $DashboardUrl
    } -ArgumentList $Url | Out-Null
}

Write-Host "Dashboard FSO : http://127.0.0.1:$UiPort"
Write-Host "Télémétrie    : $TelemetryHost`:$TelemetryPort ($FlightHz Hz / $SystemsHz Hz)"
if ($Replay) { Write-Host "Replay        : $Replay" }
Write-Host "Ctrl+C arrête le bridge."

& $PythonExe @Arguments
