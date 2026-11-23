# installer.ps1 - build a Windows Setup.exe with Forge.
# Runs build.ps1 + bundle.ps1 (staging tivi.exe + its DLLs into dist\win-x64),
# then runs Forge (expected at ..\Forge) against forge.toml to emit
# dist\installer\tivi-Setup-<version>.exe. Pass -SkipBuild to reuse an existing
# dist\win-x64.
param(
  [string]$ForgeDir = "..\Forge",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$forgeRoot   = Resolve-Path $ForgeDir
$forgeGui    = Join-Path $forgeRoot "build\forge.exe"
$uninstaller = Join-Path $forgeRoot "build\stub.exe"
$stageDir    = Join-Path $PSScriptRoot "dist\win-x64"
$outDir      = Join-Path $PSScriptRoot "dist\installer"

if (-not $SkipBuild) {
    Write-Host "[1/4] Building + bundling tivi..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed (exit $LASTEXITCODE)" }
    & (Join-Path $PSScriptRoot "bundle.ps1")
} else {
    Write-Host "[1/4] Skipping rebuild (-SkipBuild)" -ForegroundColor DarkGray
}
if (-not (Test-Path (Join-Path $stageDir "tivi.exe"))) { throw "Missing dist\win-x64\tivi.exe - run without -SkipBuild." }

Write-Host "`n[2/4] Payload staged in $stageDir" -ForegroundColor Cyan
Write-Host ("  {0} files" -f (Get-ChildItem $stageDir).Count) -ForegroundColor DarkGray

Write-Host "`n[3/4] Checking prebuilt Forge 2..." -ForegroundColor Cyan
foreach ($relative in @('build/forge.exe', 'build/stub.exe', 'build/wizard/Forge.Wizard.exe', 'build/wizard/libSkiaSharp.dll', 'build/wizard/libHarfBuzzSharp.dll')) {
    $required = Join-Path $forgeRoot $relative
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing $required. Run 'gobake build' in $forgeRoot first." }
}
$forgeVersion = & $forgeGui --version
if ($LASTEXITCODE -ne 0 -or $forgeVersion -notmatch '^forge v2\.') { throw 'Forge 2 is required. Rebuild the sibling Forge toolkit.' }

Write-Host "`n[4/4] Building Setup.exe..." -ForegroundColor Cyan
function Invoke-Forge {
    param([string[]]$ForgeArgs)
    $p = Start-Process -FilePath $forgeGui -ArgumentList $ForgeArgs -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) { throw "forge $($ForgeArgs -join ' ') failed (exit $($p.ExitCode))" }
}
Invoke-Forge @("validate", "forge.toml")
Invoke-Forge @("build", "--out", $outDir)

$setup = Get-ChildItem $outDir -Filter "tivi-Setup-*.exe" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $setup) { throw "Setup.exe not found in $outDir after build." }
Write-Host ""
Write-Host "Done. Output: $($setup.FullName)" -ForegroundColor Green
Write-Host ("  size: {0} MB" -f [math]::Round($setup.Length/1MB,1))
