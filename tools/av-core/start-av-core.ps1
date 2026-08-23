[CmdletBinding()]
param(
    [string]$HostAddress = "127.0.0.1",
    [string]$ConfigPath
)

$ErrorActionPreference = "Stop"
$ToolRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ToolRoot "..\..")).Path
$BuildRoot = Join-Path $RepoRoot "build\av-core"
$VirtualEnv = Join-Path $BuildRoot ".venv"
$PythonExe = Join-Path $VirtualEnv "Scripts\python.exe"
$BackendRoot = Join-Path $ToolRoot "backend"
$FrontendRoot = Join-Path $ToolRoot "frontend"
$FrontendDist = Join-Path $BuildRoot "frontend"
$Requirements = Join-Path $BackendRoot "requirements.lock.txt"
$RequirementStamp = Join-Path $VirtualEnv ".requirements.sha256"

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $BuildRoot "av-core.json"
}

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $PythonExe)) {
    python -m venv $VirtualEnv
}

$CurrentRequirementsHash = (Get-FileHash -LiteralPath $Requirements -Algorithm SHA256).Hash
$InstalledRequirementsHash = if (Test-Path -LiteralPath $RequirementStamp) {
    (Get-Content -LiteralPath $RequirementStamp -Raw).Trim()
} else { "" }
if ($CurrentRequirementsHash -ne $InstalledRequirementsHash) {
    & $PythonExe -m pip install --disable-pip-version-check -r $Requirements
    Set-Content -LiteralPath $RequirementStamp -Value $CurrentRequirementsHash -Encoding ascii
}

Push-Location $FrontendRoot
try {
    npm ci
    if ($LASTEXITCODE -ne 0) { throw "npm ci a échoué avec le code $LASTEXITCODE" }
    npm run build
    if ($LASTEXITCODE -ne 0) { throw "npm run build a échoué avec le code $LASTEXITCODE" }
}
finally {
    Pop-Location
}

$env:PYTHONPATH = $BackendRoot
Write-Host "AV CORE : http://$HostAddress`:<port configuré, 8080 par défaut>"
Write-Host "Config  : $ConfigPath"
& $PythonExe -m av_core --host $HostAddress --config $ConfigPath --frontend-dir $FrontendDist
