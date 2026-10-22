# Builds side-overlap.exe, irinspect.exe, sideeval.exe and runs the tests
# Fetches a pinned llvm-mingw toolchain into .tools on first use
# -Install copies the overlay to a stable per-user folder for RaceLab to launch
param([switch]$SkipTests, [switch]$Install)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$tcName = 'llvm-mingw-20261006-ucrt-x86_64'
$tcSha256 = '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'
$tc = Join-Path $root ".tools\$tcName"
$cxx = Join-Path $tc 'bin\clang++.exe'

if (-not (Test-Path $cxx)) {
  New-Item -ItemType Directory -Force (Join-Path $root '.tools') | Out-Null
  $zip = Join-Path $root ".tools\$tcName.zip"
  Write-Host "Downloading $tcName..."
  $ProgressPreference = 'SilentlyContinue'
  Invoke-WebRequest "https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/$tcName.zip" -OutFile $zip
  if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $tcSha256) { Remove-Item $zip; throw 'toolchain checksum mismatch' }
  Expand-Archive $zip -DestinationPath (Join-Path $root '.tools')
  Remove-Item $zip
}

$out = Join-Path $root 'bin'
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $root 'src'
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-static', '-s', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00', "-I$src")

function Build($name, $files, $extra) {
  $args = $common + $extra + ($files | ForEach-Object { Join-Path $root $_ }) + @('-o', (Join-Path $out $name))
  & $cxx @args
  if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
  '{0,-18} {1,8:N0} bytes' -f $name, (Get-Item (Join-Path $out $name)).Length
}

$estimator = @('src\estimator.cpp', 'src\session_info.cpp', 'src\frame.cpp')
Build 'irinspect.exe' @('src\inspect.cpp', 'src\sdk_live.cpp', 'src\ibt.cpp', 'src\recorder.cpp', 'src\frame.cpp',
  'src\session_info.cpp') @()
Build 'estimator_test.exe' (@('tests\estimator_test.cpp', 'src\ibt.cpp') + $estimator) @()
$overlay = @('src\overlay.cpp', 'src\render.cpp', 'src\settings.cpp', 'src\demo.cpp', 'src\sdk_live.cpp', 'src\ibt.cpp',
  'src\recorder.cpp') + $estimator
Build 'side-overlap.exe' $overlay @('-mwindows', '-municode', '-lshell32', '-lole32', '-luuid', '-lpsapi')
Build 'sideeval.exe' (@('src\sideeval.cpp', 'src\ibt.cpp') + $estimator) @()

if (-not $SkipTests) {
  & (Join-Path $out 'estimator_test.exe') (Join-Path $root 'tests\fixtures')
  if ($LASTEXITCODE -ne 0) { throw 'estimator tests failed' }
}

if ($Install) {
  $dest = Join-Path $env:LOCALAPPDATA 'Programs\iRacingSideOverlap'
  New-Item -ItemType Directory -Force $dest | Out-Null
  if (Get-Process side-overlap -ErrorAction SilentlyContinue) { throw 'close the running overlay before installing' }
  Copy-Item (Join-Path $out 'side-overlap.exe'), (Join-Path $out 'irinspect.exe') $dest -Force
  "installed to $dest"
}
