param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$QtRoot = ""
)

$ErrorActionPreference = "Stop"
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$executable = Join-Path $build "bin\av-ds.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable not found: $executable" }

New-Item -ItemType Directory -Force -Path $output | Out-Null
$legacyExecutable = Join-Path $output "FsoSimpitRadar.exe"
if (Test-Path -LiteralPath $legacyExecutable) {
    Remove-Item -LiteralPath $legacyExecutable -Force
}
Copy-Item -LiteralPath $executable -Destination $output -Force
$deploy = if ($QtRoot) { Join-Path $QtRoot "bin\windeployqt.exe" } else { "windeployqt.exe" }
& $deploy --release --no-translations --compiler-runtime --dir $output (Join-Path $output "av-ds.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }
if (-not (Test-Path -LiteralPath (Join-Path $output "Qt6Svg.dll"))) {
    throw "windeployqt did not deploy Qt6Svg.dll"
}

# windeployqt needs a Visual Studio developer environment to locate the CRT.
# Keep the bundle self-contained even when this script is called from an
# ordinary PowerShell session.
$redistRoot = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
$crt = Get-ChildItem -LiteralPath $redistRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $crt) { throw "MSVC x64 runtime not found under $redistRoot" }
Copy-Item -Path (Join-Path $crt "*.dll") -Destination $output -Force

$licenses = Join-Path $output "licenses"
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repository "Copying.md") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repository "Unlicense.md") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $repository "tools\radar\THIRD_PARTY_NOTICES.md") -Destination $licenses
Invoke-WebRequest -UseBasicParsing -Uri "https://www.gnu.org/licenses/lgpl-3.0.txt" -OutFile (Join-Path $licenses "Qt-LGPL-3.0.txt")
if ($QtRoot -and (Test-Path -LiteralPath (Join-Path $QtRoot "sbom"))) {
    $sbom = Join-Path $licenses "qt-sbom"
    New-Item -ItemType Directory -Force -Path $sbom | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $QtRoot "sbom") -File |
        Where-Object { $_.Name -like "qtbase-*" -or $_.Name -like "qtsvg-*" } |
        Copy-Item -Destination $sbom -Force
}
$zip = "$output.zip"
Compress-Archive -Path (Join-Path $output "*") -DestinationPath $zip -Force
Write-Output $zip
