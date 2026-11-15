# The launcher (DDDA-Coop.exe) driven for real on the test copies: each side gets its own launcher folder
# (DDDA-Coop.exe + dinput8.dll + its DDDA-Coop.ini), started with --no-launch; "Lancer le jeu" is pressed
# (WM_COMMAND), the coop.ini it wrote is checked, then the test copy's DDDA.exe is started with a bot (as
# run_pair does) and the launchers' Salon is captured. The test lines (block_saves, private appdata, test
# save, window...) are written to the copy's coop.ini first: the launcher keeps lines it does not set.
#  steam: the owner's case of 2026-11-15 (an old lan=1, a GOG game chosen, then Steam): Steam is pressed,
#         the host plays over Steam (the owner's real Steam account: presence only, no friend joins)
#  gog:   host gog1 + guest gog2 through the launcher's direct IP (the guest's address 127.0.0.1)
# Prints PASS/FAIL lines. Usage: .\suite_launcher.ps1 [-Only steam|gog]
param([string]$Only = "")
$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$build = Join-Path $PSScriptRoot "..\build"
$out = "J:\coop-test\re\runs\launcher"
$saveDir = (Get-ChildItem "${env:ProgramFiles(x86)}\Steam\userdata\*\367500" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$ownerConfig = "$env:LOCALAPPDATA\CAPCOM\DRAGONS DOGMA DARK ARISEN\config.ini"
$gogStorage = "$env:LOCALAPPDATA\GOG.com\Galaxy\Applications\49987265717041704\Storage"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class LW {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Dialog(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (p == pid && c.ToString() == "#32770" && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Text(IntPtr h) { var s = new StringBuilder(2048); GetWindowText(h, s, 2048); return s.ToString(); }
}
'@
[LW]::SetProcessDPIAware() | Out-Null
$WM_COMMAND = 0x111; $ID_LAUNCH = 106; $ID_STORE_STEAM = 128; $ID_NAV = 200

function Get-SaveHashes {
    if (Test-Path $gogStorage) { Get-ChildItem $gogStorage -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName)|$((Get-FileHash $_.FullName).Hash)" } }
    Get-ChildItem $saveDir -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName)|$((Get-FileHash $_.FullName).Hash)" }
    "$ownerConfig|$((Get-FileHash $ownerConfig).Hash)"
}
function Shot($h, $path) { # the window's own drawing (PrintWindow), whatever covers it
    $r = New-Object LW+RECT; [LW]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [LW]::PrintWindow($h, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc); $bmp.Save($path); $g.Dispose(); $bmp.Dispose()
}
# a game copy: the mod's private files gone (hard links are never written through), the test lines in coop.ini
function Prepare-Copy($dir, $window, [string[]]$lines, $bot, [switch]$gog) {
    foreach ($f in "dinput8.dll", "coop.ini", "bot.txt", "coop.log", "coop_status.txt", "bridge_in.txt", "bridge_out.txt") { Remove-Item (Join-Path $dir $f) -ErrorAction SilentlyContinue }
    Get-ChildItem $dir -Filter "coop_avatar_*.bmp" -ErrorAction SilentlyContinue | Remove-Item
    (@("p2_pad=1", "idle_pad=3", "block_saves=1", "multi_instance=1", "keep_active=1", "private_appdata=1", "window=$window") + $lines) | Set-Content (Join-Path $dir "coop.ini") -Encoding ascii
    $cfgDir = Join-Path $dir "coop_appdata\CAPCOM\DRAGONS DOGMA DARK ARISEN"
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    (Get-Content $ownerConfig) -replace '^FullScreen=ON$', 'FullScreen=OFF' | Set-Content (Join-Path $cfgDir "config.ini") -Encoding ascii
    if ($gog) { # GOG: one more A at 93 s for "connection failed" (as run_pair -Gog)
        $steps = @(Get-Content $bot | ForEach-Object { $i = 0 } { $l = ($_ -split '#')[0].Trim(); if ($l -match '^\d+') { [pscustomobject]@{ T = [int64]($l -split '\s+')[0]; I = $i++; L = $l } } })
        if ($steps | Where-Object { $_.L -match '^82000\s+0\s+1000' }) { $steps += [pscustomobject]@{ T = 93000; I = 100000; L = "93000 0 1000" }, [pscustomobject]@{ T = 93300; I = 100001; L = "93300 0 0" } }
        $steps | Sort-Object T, I | ForEach-Object { $_.L } | Set-Content (Join-Path $dir "bot.txt") -Encoding ascii
    } else { Copy-Item $bot (Join-Path $dir "bot.txt") }
}
# a launcher of its own: folder, exe + dll, settings; returns its process (window ready)
function Start-Launcher($name, [string[]]$settings, [string[]]$extraArgs, $x) {
    $d = Join-Path $out "launcher_$name"
    New-Item -ItemType Directory -Force "$d\docs" | Out-Null
    Copy-Item (Join-Path $build "DDDA-Coop.exe"), (Join-Path $build "dinput8.dll") $d
    [IO.File]::WriteAllText("$d\DDDA-Coop.ini", "[launcher]`r`nhelp_seen=1`r`ngog_help_seen=1`r`ncheck_updates=0`r`nlang=FR`r`n" + ($settings -join "`r`n") + "`r`n")
    $p = Start-Process "$d\DDDA-Coop.exe" -ArgumentList (@('--docs', "$d\docs", '--no-launch') + $extraArgs) -PassThru
    for ($k = 0; $k -lt 40 -and $p.MainWindowHandle -eq 0; $k++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
    Start-Sleep 1
    [LW]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $x, 40, 0, 0, 0x0001 -bor 0x0004) | Out-Null # SWP_NOSIZE | SWP_NOZORDER
    $p
}
function Cmd($p, $id) { [LW]::SendMessageW($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) | Out-Null }
# "Lancer le jeu": PostMessage, then the backup's question (no saves in the test's storage) answered Yes;
# any other box is reported
function Press-Launch($p) {
    [LW]::PostMessageW($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$ID_LAUNCH, [IntPtr]::Zero) | Out-Null
    $said = @()
    for ($k = 0; $k -lt 20; $k++) {
        Start-Sleep -Milliseconds 300
        $dlg = [LW]::Dialog([uint32]$p.Id)
        if ($dlg -eq [IntPtr]::Zero) { if ($k -ge 6) { break } else { continue } }
        $text = [LW]::Text([LW]::GetDlgItem($dlg, 0xFFFF))
        $said += $text -replace "`n", " "
        $yes = [LW]::GetDlgItem($dlg, 6)
        $btn = if ($yes -ne [IntPtr]::Zero) { 6 } else { 1 }
        [LW]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]$btn, [IntPtr]::Zero) | Out-Null
        $k = 0
    }
    $said
}
$checks = @()
function Check($ok, $what) { if ($ok) { "PASS  $what" } else { "FAIL  $what" } }
$before = Get-SaveHashes

# ---- steam: the owner's case ----
if (-not $Only -or $Only -eq "steam") {
    $fake = Join-Path $out "fake_steam"   # a Steam folder whose library is the Steam test copy
    New-Item -ItemType Directory -Force "$fake\steamapps\common" | Out-Null
    if (-not (Test-Path "$fake\steamapps\common\DDDA")) { New-Item -ItemType Junction -Path "$fake\steamapps\common\DDDA" -Target 'J:\coop-test\DDDA' | Out-Null }
    Prepare-Copy 'J:\coop-test\DDDA' "0,0,1920,1080" @("test_save=J:\coop-test\saves\guild.sav") (Join-Path $tests "bot_load_game_p2.txt")
    $l = Start-Launcher "steam" @("role=host", "lan=1", "game=J:\coop-test\gog1") @('--steam', $fake) 100
    Shot $l.MainWindowHandle "$out\steam_0_start_gog.png"
    Cmd $l $ID_STORE_STEAM; Start-Sleep 2
    Cmd $l ($ID_NAV + 3); Shot $l.MainWindowHandle "$out\steam_1_settings_after_steam.png"
    $said = Press-Launch $l
    $said | ForEach-Object { "   launcher said: $_" }
    $ini = Get-Content 'J:\coop-test\DDDA\coop.ini'
    $checks += Check ($ini -contains "transport=steam" -and -not ($ini -match '^transport=udp')) "steam: coop.ini written with transport=steam after GOG then Steam (old lan=1 ignored)"
    $checks += Check ($ini -contains "role=host" -and $ini -contains "block_saves=1" -and $ini -contains "private_appdata=1") "steam: role=host written, the test lines kept"
    $checks += Check ((Test-Path 'J:\coop-test\DDDA\dinput8.dll') -and (Select-String -Path 'J:\coop-test\DDDA\dinput8.dll' -Pattern 'ddda-pawn-coop loaded' -SimpleMatch -Quiet)) "steam: the launcher installed the mod's dinput8.dll"
    $g = Start-Process 'J:\coop-test\DDDA\DDDA.exe' -WorkingDirectory 'J:\coop-test\DDDA' -PassThru
    $t0 = Get-Date; $guard = $false
    while (((Get-Date) - $t0).TotalSeconds -lt 130) {
        Start-Sleep 1
        if (-not $guard -and (Test-Path 'J:\coop-test\DDDA\coop.log') -and (Select-String -Path 'J:\coop-test\DDDA\coop.log' -Pattern 'save-guard: ACTIVE' -Quiet)) { $guard = $true }
        if (-not $guard -and ((Get-Date) - $t0).TotalSeconds -gt 25) { "   steam: save guard not active after 25 s, stopping"; break }
        if ($g.HasExited) { break }
    }
    Cmd $l ($ID_NAV + 1); Start-Sleep 2; Shot $l.MainWindowHandle "$out\steam_2_salon.png"
    Cmd $l ($ID_NAV + 0); Start-Sleep 1; Shot $l.MainWindowHandle "$out\steam_3_play.png"
    $exited = $g.HasExited
    if (-not $exited) { Stop-Process -Id $g.Id -Force; $g.WaitForExit(10000) | Out-Null }
    Copy-Item 'J:\coop-test\DDDA\coop.log' "$out\steam_host.log" -ErrorAction SilentlyContinue
    $log = Get-Content "$out\steam_host.log" -ErrorAction SilentlyContinue
    $checks += Check $guard "steam: save guard active"
    $checks += Check (-not $exited) "steam: the game still running at the end"
    $checks += Check (($log -match 'host over Steam P2P').Count -gt 0 -and ($log -match 'steam: link up \(real Steam').Count -gt 0 -and -not ($log -match 'net: host, listening on')) "steam: the mod ran over Steam (coop.log)"
    $checks += Check (Test-Path 'J:\coop-test\DDDA\coop_avatar_0.bmp') "steam: the host's Steam picture written for the Salon (coop_avatar_0.bmp)"
    Stop-Process -Id $l.Id -ErrorAction SilentlyContinue
}

# ---- gog: host + guest through the launcher's direct IP ----
if (-not $Only -or $Only -eq "gog") {
    Prepare-Copy 'J:\coop-test\gog1' "0,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guild.sav") (Join-Path $tests "bot_load_game_p2.txt") -gog
    Prepare-Copy 'J:\coop-test\gog2' "-1920,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guest_arisen.sav") (Join-Path $tests "shared_guest_walk.txt") -gog
    $lh = Start-Launcher "gog_host" @("role=host", "game=J:\coop-test\gog1") @() 100
    $lg = Start-Launcher "gog_guest" @("role=guest", "game=J:\coop-test\gog2", "host_code=127.0.0.1") @() 1000
    foreach ($p in $lh, $lg) { Press-Launch $p | ForEach-Object { "   launcher said: $_" } }
    $hi = Get-Content 'J:\coop-test\gog1\coop.ini'; $gi = Get-Content 'J:\coop-test\gog2\coop.ini'
    $checks += Check ($hi -contains "role=host" -and $hi -contains "transport=udp" -and $hi -contains "port=27101") "gog: host's coop.ini: role=host, transport=udp, port=27101"
    $checks += Check ($gi -contains "role=guest" -and $gi -contains "transport=udp" -and $gi -contains "peer=127.0.0.1") "gog: guest's coop.ini: role=guest, transport=udp, peer=127.0.0.1"
    $gh = Start-Process 'J:\coop-test\gog1\DDDA.exe' -WorkingDirectory 'J:\coop-test\gog1' -PassThru; Start-Sleep 5
    $gg = Start-Process 'J:\coop-test\gog2\DDDA.exe' -WorkingDirectory 'J:\coop-test\gog2' -PassThru
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 200) { Start-Sleep 2; if ($gh.HasExited -or $gg.HasExited) { break } }
    foreach ($p in $lh, $lg) { Cmd $p ($ID_NAV + 1) }
    Start-Sleep 2
    Shot $lh.MainWindowHandle "$out\gog_host_salon.png"; Shot $lg.MainWindowHandle "$out\gog_guest_salon.png"
    Cmd $lg ($ID_NAV + 0); Start-Sleep 1; Shot $lg.MainWindowHandle "$out\gog_guest_play.png"
    $ex = $gh.HasExited -or $gg.HasExited
    foreach ($p in $gh, $gg) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null } }
    Copy-Item 'J:\coop-test\gog1\coop.log' "$out\gog_host.log" -ErrorAction SilentlyContinue
    Copy-Item 'J:\coop-test\gog2\coop.log' "$out\gog_guest.log" -ErrorAction SilentlyContinue
    $glog = Get-Content "$out\gog_guest.log" -ErrorAction SilentlyContinue
    $checks += Check (-not $ex) "gog: both games still running at the end"
    $checks += Check (($glog -match 'net: pings').Count -gt 0 -and ((Get-Content "$out\gog_host.log" -ErrorAction SilentlyContinue) -match 'net: guest connected from').Count -gt 0) "gog: the guest reached the host (guest's pings, host's 'guest connected')"
    $checks += Check (($glog -match "host's save version \d+ arrived").Count -gt 0) "gog: the guest got the host's world (coop.log)"
    foreach ($p in $lh, $lg) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue }
}

$checks += Check (-not (Compare-Object $before (Get-SaveHashes))) "owner's saves (Steam, GOG Galaxy) and game config unchanged"
$checks | ForEach-Object { "   $_" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
"launcher suite: $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
"outputs: $out"
if ($failed) { exit 1 }
