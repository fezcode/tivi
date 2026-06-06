# bundle.ps1 - stage a standalone tivi that runs without the MSYS2 environment.
# Copies build\tivi.exe plus the full closure of mingw64 DLLs it depends on into
# dist\win-x64. Run after build.ps1.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$exe = Join-Path $root 'build\tivi.exe'
if (-not (Test-Path $exe)) { throw "build\tivi.exe not found - run .\build.ps1 first" }

$gccDir = Split-Path (Get-Command gcc).Source
$objdump = Join-Path $gccDir 'objdump.exe'
if (-not (Test-Path $objdump)) { throw "objdump not found next to gcc" }

$stage = Join-Path $root 'dist\win-x64'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe $stage -Force

# Recursively resolve DLL imports; copy any that live in the mingw64 bin dir.
$seen = @{}
function Resolve-Deps([string]$file) {
    $deps = & $objdump -p $file 2>$null | Select-String 'DLL Name:\s*(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    foreach ($d in $deps) {
        if ($seen.ContainsKey($d)) { continue }
        $seen[$d] = $true
        $src = Join-Path $gccDir $d
        if (Test-Path $src) {                       # skip Windows system DLLs (not in mingw64)
            Copy-Item $src $stage -Force
            Resolve-Deps $src
        }
    }
}
Resolve-Deps $exe

$dlls = (Get-ChildItem $stage -Filter *.dll).Count
Write-Host ("Bundled tivi.exe + {0} DLLs into {1}" -f $dlls, $stage) -ForegroundColor Green
Write-Host "This folder is self-contained and can be copied anywhere."
