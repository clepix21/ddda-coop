# Builds build\dinput8.dll (32-bit) with llvm-mingw and MinHook source.
# Usage: .\build.ps1 [-Toolchain <llvm-mingw dir>] [-MinHook <minhook dir>]
param(
    [string]$Toolchain = "J:\coop-test\tools\llvm-mingw-20260922-ucrt-x86_64",
    [string]$MinHook = "J:\coop-test\tools\minhook"
)
$ErrorActionPreference = "Stop"
$cxx = Join-Path $Toolchain "bin\i686-w64-mingw32-clang++.exe"
$cc = Join-Path $Toolchain "bin\i686-w64-mingw32-clang.exe"
$out = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $out | Out-Null

$mhSrc = @("src\buffer.c", "src\hook.c", "src\trampoline.c", "src\hde\hde32.c") | ForEach-Object { Join-Path $MinHook $_ }
$objs = @()
foreach ($c in $mhSrc) {
    $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($c) + ".o")
    & $cc -c -O2 -I (Join-Path $MinHook "include") $c -o $o
    if ($LASTEXITCODE) { throw "compile failed: $c" }
    $objs += $o
}
$src = Get-ChildItem (Join-Path $PSScriptRoot "src\*.cpp") | ForEach-Object { $_.FullName }
& $cxx -shared -O2 -std=c++17 -Wall -Wno-unused-function -static -static-libgcc -static-libstdc++ `
    -I (Join-Path $MinHook "include") $src $objs `
    "-Wl,--kill-at" "-Wl,--enable-stdcall-fixup" -lws2_32 `
    -o (Join-Path $out "dinput8.dll")
if ($LASTEXITCODE) { throw "link failed" }
Get-Item (Join-Path $out "dinput8.dll") | Select-Object Name, Length
