# Builds wkModMenu.dll (a 32-bit WormKit module for Worms Armageddon 3.8.1) with MinGW (32-bit gcc).
# Usage:  .\build.ps1 [-Gcc C:\MinGW\bin\gcc.exe]
param([string]$Gcc = 'C:\MinGW\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mh = Join-Path $here 'third_party\minhook'
$opt = @('-O2', '-Wall', '-Wextra', '-std=gnu99', '-msse2', '-mfpmath=sse', '-fno-math-errno', '-fno-trapping-math')
$out = Join-Path $here 'wkModMenu.dll'
$sources = @((Join-Path $here 'src\wkModMenu.c'), (Join-Path $mh 'src\buffer.c'), (Join-Path $mh 'src\hook.c'),
             (Join-Path $mh 'src\trampoline.c'), (Join-Path $mh 'src\hde\hde32.c'))
$buildArgs = @('-shared') + $opt + @('-mstackrealign', '-static-libgcc', '-s', "-I$mh\include", "-I$here\src") + $sources +
             @('-lgdi32', '-o', $out)
$ErrorActionPreference = 'Continue'   # (compiler warnings on stderr are not build failures)
& $Gcc @buildArgs
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw 'wkModMenu build failed' }
Get-FileHash -LiteralPath $out -Algorithm SHA256
