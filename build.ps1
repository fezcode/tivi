# Build tivi — produces build\tivi.exe.
# Links raylib (window/input/GPU/audio), FFmpeg (libav*) for decode/scale/resample,
# and libass for subtitles. FFmpeg is linked dynamically against the MSYS2 mingw64
# DLLs, so those DLLs must be reachable at run time (they are inside the mingw64
# shell; `installer.ps1` bundles them for distribution).
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$gcc = (Get-Command gcc -ErrorAction SilentlyContinue).Source
if (-not $gcc) { throw "gcc not found on PATH (install MSYS2 mingw64: 'pacman -S mingw-w64-x86_64-gcc')" }
$pkg = (Get-Command pkg-config -ErrorAction SilentlyContinue).Source
if (-not $pkg) { throw "pkg-config not found on PATH ('pacman -S mingw-w64-x86_64-pkgconf')" }

$pkgs = 'raylib libavformat libavcodec libavutil libswscale libswresample libavfilter libass'
$cf = (& cmd /c "pkg-config --cflags $pkgs") -split '\s+' | Where-Object { $_ }
$lf = (& cmd /c "pkg-config --libs $pkgs")   -split '\s+' | Where-Object { $_ }
if (-not $lf) { throw "pkg-config could not resolve deps. Install: 'pacman -S mingw-w64-x86_64-raylib mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-libass'" }

$flags   = @('-O2','-Wall','-Wextra','-Wno-unused-parameter','-std=c11','-Isrc') + $cf
# -mwindows → GUI subsystem (no console window). CLI modes attach to the parent
# console at run time. Windows system libs for raylib + the native dialog/DWM glue.
$winlibs = '-mwindows','-lopengl32','-lgdi32','-lwinmm','-lcomdlg32','-lole32','-loleaut32','-luser32','-ldwmapi','-lshell32','-lm'

New-Item -ItemType Directory -Force -Path build | Out-Null
$srcs = 'main','player','audio_out','subs','playlist','mediakeys','singleinst','osvideo','viconfig','yuvtex'

# Incremental: any .h change invalidates every .o (avoids stale-struct corruption).
$headers = Get-ChildItem src\*.h -ErrorAction SilentlyContinue
$newestHeader = if ($headers) { ($headers | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime } else { [datetime]::MinValue }
$objs = @()
foreach ($s in $srcs) {
    $src = "src\$s.c"; $obj = "build\$s.o"
    $upToDate = (Test-Path $obj) -and
                ((Get-Item $obj).LastWriteTime -gt (Get-Item $src).LastWriteTime) -and
                ((Get-Item $obj).LastWriteTime -gt $newestHeader)
    if ($upToDate) { Write-Output "up-to-date $s"; $objs += $obj; continue }
    Write-Output "compiling $s"
    & $gcc @flags -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
    $objs += $obj
}

# Embed the Windows .exe icon (optional — needs windres + assets\tivi.ico).
$rcObj = $null
$windres = (Get-Command windres -ErrorAction SilentlyContinue).Source
if ($windres -and (Test-Path 'src\app.rc')) {
    Write-Output 'compiling resource app.rc'
    & $windres 'src\app.rc' -o 'build\app_rc.o'
    if ($LASTEXITCODE -eq 0) { $rcObj = 'build\app_rc.o' }
}

Write-Output 'linking tivi.exe'
$linkObjs = if ($rcObj) { $objs + $rcObj } else { $objs }
& $gcc @linkObjs -o build\tivi.exe @lf @winlibs
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Write-Output ('built ' + (Resolve-Path build\tivi.exe))
