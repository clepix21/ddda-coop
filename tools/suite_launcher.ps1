# The launcher (DDDA-Coop.exe) driven for real on the test copies: each side gets its own launcher folder
# (DDDA-Coop.exe + dinput8.dll + its DDDA-Coop.ini), started with --no-launch; "Lancer le jeu" is pressed
# (WM_COMMAND), the coop.ini it wrote is checked, then the test copy's DDDA.exe is started with a bot (as
# run_pair does) and the launchers' Salon is captured. The test lines (block_saves, private appdata, test
# save, window...) are written to the copy's coop.ini first: the launcher keeps lines it does not set.
#  steam: the owner's case of 2026-11-15 (an old lan=1, a GOG game chosen, then Steam): Steam is pressed,
#         the host plays over Steam (the owner's real Steam account: presence only, no friend joins)
#  gog:   host gog1 + guest gog2 through the launcher's direct IP (the guest's address 127.0.0.1)
#  mixed: a Steam host with "Play without Steam" on (direct_ip=1) and a GOG guest (gog2), by 127.0.0.1
#  goglaunch: the launcher starts GOG's game itself (no --no-launch): gog1 given as GOG's install (--gog),
#         a fake Galaxy storage (--gog-storage) backed up into the test's Documents
#  firstrun: no DDDA-Coop.ini: with only a GOG game, the help then the GOG help come by themselves, once,
#         GOG chosen; with both games, Steam chosen and no GOG help; then the pages in the 5 languages
# Prints PASS/FAIL lines. Usage: .\suite_launcher.ps1 [-Only steam|gog|mixed|goglaunch|firstrun]
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
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  // every child's text, and every list box's lines (the state line and the events)
  public static string[] Texts(IntPtr top) {
    var all = new System.Collections.Generic.List<string>();
    EnumChildWindows(top, (h, l) => { var c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() == "ListBox") { int n = (int)SendMessageW(h, 0x018B, IntPtr.Zero, IntPtr.Zero);
        for (int i = 0; i < n; i++) { var b = new StringBuilder(4096); SendMessageW(h, 0x0189, (IntPtr)i, b); all.Add(b.ToString()); } }
      else all.Add(Text(h));
      return true; }, IntPtr.Zero);
    return all.ToArray();
  }
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
function Start-Launcher($name, [string[]]$settings, [string[]]$extraArgs, $x, [switch]$launch, [switch]$noIni, [switch]$keep) {
    $d = Join-Path $out "launcher_$name"
    New-Item -ItemType Directory -Force "$d\docs" | Out-Null
    Copy-Item (Join-Path $build "DDDA-Coop.exe"), (Join-Path $build "dinput8.dll") $d
    if ($noIni) { if (-not $keep) { Remove-Item "$d\DDDA-Coop.ini" -ErrorAction SilentlyContinue } }
    else { # the defaults, unless $settings sets that key (the first one counts in an ini)
        $keys = $settings | ForEach-Object { ($_ -split '=')[0] }
        $defs = @("help_seen=1", "gog_help_seen=1", "check_updates=0", "lang=FR") | Where-Object { $keys -notcontains ($_ -split '=')[0] }
        [IO.File]::WriteAllText("$d\DDDA-Coop.ini", "[launcher]`r`n" + ((@($defs) + $settings) -join "`r`n") + "`r`n") }
    $a = @('--docs', "$d\docs") + $extraArgs
    if (-not $launch) { $a += '--no-launch' }
    $p = Start-Process "$d\DDDA-Coop.exe" -ArgumentList $a -PassThru
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
# the boxes a launcher shows by itself within $sec seconds: "title | text", each closed (No, else OK)
function Read-Boxes($p, $sec) {
    $said = @(); $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $sec) {
        Start-Sleep -Milliseconds 300
        $dlg = [LW]::Dialog([uint32]$p.Id)
        if ($dlg -eq [IntPtr]::Zero) { continue }
        $said += "$([LW]::Text($dlg)) | $(([LW]::Text([LW]::GetDlgItem($dlg, 0xFFFF))) -replace "`n", " ")"
        $no = [LW]::GetDlgItem($dlg, 7)
        $btn = if ($no -ne [IntPtr]::Zero) { 7 } else { 2 } # IDCANCEL closes an OK-only box
        [LW]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]$btn, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 500
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
New-Item -ItemType Directory -Force (Join-Path $out "fake_galaxy_storage\sub") | Out-Null; Set-Content (Join-Path $out "fake_galaxy_storage\sub\marker.txt") "fake Galaxy storage for the launcher test" -Encoding ascii
if (-not $Only -or $Only -eq "gog") {
    Prepare-Copy 'J:\coop-test\gog1' "0,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guild.sav") (Join-Path $tests "bot_load_game_p2.txt") -gog
    Prepare-Copy 'J:\coop-test\gog2' "-1920,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guest_arisen.sav") (Join-Path $tests "shared_guest_walk.txt") -gog
    $lh = Start-Launcher "gog_host" @("role=host", "game=J:\coop-test\gog1") @('--gog-storage', (Join-Path $out "fake_galaxy_storage")) 100
    $lg = Start-Launcher "gog_guest" @("role=guest", "game=J:\coop-test\gog2", "host_code=127.0.0.1") @('--gog-storage', (Join-Path $out "fake_galaxy_storage")) 1000
    foreach ($p in $lh, $lg) { Press-Launch $p | ForEach-Object { "   launcher said: $_" } }
    $hi = Get-Content 'J:\coop-test\gog1\coop.ini'; $gi = Get-Content 'J:\coop-test\gog2\coop.ini'
    $checks += Check ($hi -contains "role=host" -and $hi -contains "transport=udp" -and $hi -contains "port=27101") "gog: host's coop.ini: role=host, transport=udp, port=27101"
    $checks += Check ($gi -contains "role=guest" -and $gi -contains "transport=udp" -and $gi -contains "peer=127.0.0.1") "gog: guest's coop.ini: role=guest, transport=udp, peer=127.0.0.1"
    $gh = Start-Process 'J:\coop-test\gog1\DDDA.exe' -WorkingDirectory 'J:\coop-test\gog1' -PassThru; Start-Sleep 5
    $gg = Start-Process 'J:\coop-test\gog2\DDDA.exe' -WorkingDirectory 'J:\coop-test\gog2' -PassThru
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 200) { Start-Sleep 2; if ($gh.HasExited -or $gg.HasExited) { break } }
    $hw = [LW]::Texts($lh.MainWindowHandle); $gw = [LW]::Texts($lg.MainWindowHandle)
    $hw | Where-Object { $_ -match '^\d\d:\d\d:\d\d|^\S\s\s' } | ForEach-Object { "   host launcher: $_" }
    $gw | Where-Object { $_ -match '^\d\d:\d\d:\d\d|^\S\s\s' } | ForEach-Object { "   guest launcher: $_" }
    $checks += Check (@($hw -match 'Pr.t en IP directe, port UDP 27101').Count -gt 0 -and @($hw -match 'ami connect').Count -gt 0) "gog: host's launcher: 'ready over direct IP', then '1 friend connected'"
    $checks += Check (@($gw -match "IP directe : 127\.0\.0\.1").Count -gt 0 -and @($gw -match 'H.te trouv').Count -gt 0 -and @($gw -match "Monde de l'h.te re.u").Count -gt 0) "gog: guest's launcher: connecting to 127.0.0.1, host found, world received"
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

# ---- mixed: a Steam host with "Play without Steam" on, a GOG guest, through direct IP ----
if (-not $Only -or $Only -eq "mixed") {
    $fake = Join-Path $out "fake_steam"
    New-Item -ItemType Directory -Force "$fake\steamapps\common" | Out-Null
    if (-not (Test-Path "$fake\steamapps\common\DDDA")) { New-Item -ItemType Junction -Path "$fake\steamapps\common\DDDA" -Target 'J:\coop-test\DDDA' | Out-Null }
    $storage = Join-Path $out "fake_galaxy_storage"; New-Item -ItemType Directory -Force "$storage\sub" | Out-Null
    Set-Content "$storage\sub\marker.txt" "fake Galaxy storage for the launcher test" -Encoding ascii
    Prepare-Copy 'J:\coop-test\DDDA' "0,0,1920,1080" @("test_save=J:\coop-test\saves\guild.sav") (Join-Path $tests "bot_load_game_p2.txt")
    Prepare-Copy 'J:\coop-test\gog2' "-1920,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guest_arisen.sav") (Join-Path $tests "shared_guest_walk.txt") -gog
    $lh = Start-Launcher "mixed_host" @("role=host", "direct_ip=1", "game=J:\coop-test\DDDA") @('--steam', $fake) 100
    $lg = Start-Launcher "mixed_guest" @("role=guest", "game=J:\coop-test\gog2", "host_code=127.0.0.1") @('--gog-storage', $storage) 1000
    foreach ($p in $lh, $lg) { Press-Launch $p | ForEach-Object { "   launcher said: $_" } }
    $hi = Get-Content 'J:\coop-test\DDDA\coop.ini'; $gi = Get-Content 'J:\coop-test\gog2\coop.ini'
    $checks += Check ($hi -contains "role=host" -and $hi -contains "transport=udp" -and $hi -contains "port=27101") "mixed: Steam host's coop.ini: transport=udp (Play without Steam on), port=27101"
    $checks += Check ($gi -contains "role=guest" -and $gi -contains "transport=udp" -and $gi -contains "peer=127.0.0.1") "mixed: GOG guest's coop.ini: peer=127.0.0.1"
    $gh = Start-Process 'J:\coop-test\DDDA\DDDA.exe' -WorkingDirectory 'J:\coop-test\DDDA' -PassThru; Start-Sleep 5
    $gg = Start-Process 'J:\coop-test\gog2\DDDA.exe' -WorkingDirectory 'J:\coop-test\gog2' -PassThru
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 200) { Start-Sleep 2; if ($gh.HasExited -or $gg.HasExited) { break } }
    $hw = [LW]::Texts($lh.MainWindowHandle); $gw = [LW]::Texts($lg.MainWindowHandle)
    foreach ($p in $lh, $lg) { Cmd $p ($ID_NAV + 1) }
    Start-Sleep 2
    Shot $lh.MainWindowHandle "$out\mixed_host_salon.png"; Shot $lg.MainWindowHandle "$out\mixed_guest_salon.png"
    $ex = $gh.HasExited -or $gg.HasExited
    foreach ($p in $gh, $gg) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null } }
    Copy-Item 'J:\coop-test\DDDA\coop.log' "$out\mixed_host.log" -ErrorAction SilentlyContinue
    Copy-Item 'J:\coop-test\gog2\coop.log' "$out\mixed_guest.log" -ErrorAction SilentlyContinue
    $hlog = Get-Content "$out\mixed_host.log" -ErrorAction SilentlyContinue; $glog = Get-Content "$out\mixed_guest.log" -ErrorAction SilentlyContinue
    $checks += Check (-not $ex) "mixed: both games still running at the end"
    $checks += Check (@($hlog -match 'TimeDateStamp 0x5A314C31').Count -gt 0 -and @($glog -match 'TimeDateStamp 0x5936ABAB').Count -gt 0) "mixed: the host ran Steam's exe, the guest GOG's"
    $checks += Check (@($hlog -match 'net: guest connected from').Count -gt 0 -and @($glog -match "host's save version \d+ arrived").Count -gt 0) "mixed: the GOG guest reached the Steam host and got its world"
    $checks += Check (@($gw -match 'H.te trouv').Count -gt 0 -and @($hw -match 'ami connect').Count -gt 0) "mixed: both launchers say so (host found / friend connected)"
    foreach ($p in $lh, $lg) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue }
}
# ---- goglaunch: the launcher starts GOG's DDDA.exe itself ----
if (-not $Only -or $Only -eq "goglaunch") {
    $others = @(Get-Process DDDA -ErrorAction SilentlyContinue).Count
    $storage = Join-Path $out "fake_galaxy_storage"
    New-Item -ItemType Directory -Force "$storage\sub" | Out-Null
    Set-Content "$storage\sub\marker.txt" "fake Galaxy storage for the launcher test" -Encoding ascii
    $empty = Join-Path $out "no_steam"; New-Item -ItemType Directory -Force $empty | Out-Null
    Prepare-Copy 'J:\coop-test\gog1' "0,0,1920,1080" @("private_storage=1", "test_save=J:\coop-test\saves\guild.sav") (Join-Path $tests "bot_load_game_p2.txt") -gog
    Remove-Item 'J:\coop-test\gog1\coop.prev.log' -ErrorAction SilentlyContinue
    Set-Content 'J:\coop-test\gog1\coop.log' 'the game before (crashed)' -Encoding ascii # the mod keeps it as coop.prev.log
    $l = Start-Launcher "goglaunch" @("role=host") @('--steam', $empty, '--gog', 'J:\coop-test\gog1', '--gog-storage', $storage) 100 -launch
    $said = @(Press-Launch $l)
    $said | ForEach-Object { "   launcher said: $_" }
    $g = $null
    for ($k = 0; $k -lt 40 -and -not $g; $k++) { Start-Sleep -Milliseconds 500; $g = Get-Process DDDA -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'J:\coop-test\gog1\DDDA.exe' } | Select-Object -First 1 }
    $checks += Check ($null -ne $g) "goglaunch: the launcher started gog1\DDDA.exe itself"
    $checks += Check (@(Get-Process DDDA -ErrorAction SilentlyContinue | Where-Object { $_.Path -ne 'J:\coop-test\gog1\DDDA.exe' }).Count -eq $others) "goglaunch: no other game started (no steam://, not the owner's install)"
    $checks += Check ($said.Count -eq 0) "goglaunch: no question or error from the launcher"
    $bk = @(Get-ChildItem "$out\launcher_goglaunch\docs\DDDA Coop\Sauvegardes\*\GOG\sub\marker.txt" -ErrorAction SilentlyContinue)
    $checks += Check ($bk.Count -eq 1) "goglaunch: Galaxy's storage copied to Documents\DDDA Coop\Sauvegardes\<date>\GOG"
    $guard = $false; $t0 = Get-Date
    while ($g -and ((Get-Date) - $t0).TotalSeconds -lt 120) {
        Start-Sleep 1
        if (-not $guard -and (Test-Path 'J:\coop-test\gog1\coop.log') -and (Select-String -Path 'J:\coop-test\gog1\coop.log' -Pattern 'save-guard: ACTIVE' -Quiet)) { $guard = $true }
        if ($g.HasExited) { break }
    }
    Cmd $l ($ID_NAV + 0); Start-Sleep 2; Shot $l.MainWindowHandle "$out\goglaunch_play.png"
    Cmd $l ($ID_NAV + 1); Start-Sleep 1; Shot $l.MainWindowHandle "$out\goglaunch_salon.png"
    $exited = $g -and $g.HasExited
    if ($g -and -not $g.HasExited) { Stop-Process -Id $g.Id -Force; $g.WaitForExit(10000) | Out-Null }
    Copy-Item 'J:\coop-test\gog1\coop.log' "$out\goglaunch.log" -ErrorAction SilentlyContinue
    $log = Get-Content "$out\goglaunch.log" -ErrorAction SilentlyContinue
    $checks += Check ($guard -and -not $exited) "goglaunch: the mod loaded (save guard active), game still running after 2 min"
    $checks += Check (($log -match 'net: host, listening on port 27101').Count -gt 0) "goglaunch: the host listens on UDP 27101"
    $checks += Check ((Get-Content 'J:\coop-test\gog1\coop.prev.log' -ErrorAction SilentlyContinue) -contains 'the game before (crashed)') "goglaunch: the previous game's log kept as coop.prev.log"
    Stop-Process -Id $l.Id -ErrorAction SilentlyContinue
}

# ---- firstrun: no DDDA-Coop.ini ----
if (-not $Only -or $Only -eq "firstrun") {
    $empty = Join-Path $out "no_steam"; New-Item -ItemType Directory -Force $empty | Out-Null
    $fake = Join-Path $out "fake_steam"
    New-Item -ItemType Directory -Force "$fake\steamapps\common" | Out-Null
    if (-not (Test-Path "$fake\steamapps\common\DDDA")) { New-Item -ItemType Junction -Path "$fake\steamapps\common\DDDA" -Target 'J:\coop-test\DDDA' | Out-Null }
    $nostore = Join-Path $out "no_storage"
    # only GOG's game
    $l = Start-Launcher "first_gog" @() @('--steam', $empty, '--gog', 'J:\coop-test\gog1', '--gog-storage', $nostore) 100 -noIni
    $said = @(Read-Boxes $l 8)
    $said | ForEach-Object { "   first start (GOG only) showed: $_" }
    $st = [LW]::Text([LW]::GetDlgItem($l.MainWindowHandle, 129))
    Stop-Process -Id $l.Id; Start-Sleep 1
    $ini = Get-Content "$out\launcher_first_gog\DDDA-Coop.ini" -ErrorAction SilentlyContinue
    $checks += Check ((@($said | Where-Object { $_ -like '*Aide*' }).Count -eq 1) -and (@($said | Where-Object { $_ -like '*version GOG*' }).Count -eq 1)) "firstrun: GOG only: the help and the GOG help shown by themselves, once each"
    $checks += Check ($st -like '*choisi*') "firstrun: GOG only: GOG chosen in Parametres (button: $st)"
    $checks += Check ($ini -contains 'help_seen=1' -and $ini -contains 'gog_help_seen=1') "firstrun: help_seen and gog_help_seen written"
    $l = Start-Launcher "first_gog" @() @('--steam', $empty, '--gog', 'J:\coop-test\gog1', '--gog-storage', $nostore) 100 -noIni -keep
    $again = @(Read-Boxes $l 6)
    Stop-Process -Id $l.Id; Start-Sleep 1
    $checks += Check ($again.Count -eq 0) "firstrun: second start: no box by itself ($($again -join ' / '))"
    # both games
    $l = Start-Launcher "first_both" @() @('--steam', $fake, '--gog', 'J:\coop-test\gog1', '--gog-storage', $nostore) 100 -noIni
    $said = @(Read-Boxes $l 8)
    $said | ForEach-Object { "   first start (both) showed: $_" }
    $st = [LW]::Text([LW]::GetDlgItem($l.MainWindowHandle, 128))
    Stop-Process -Id $l.Id; Start-Sleep 1
    $checks += Check ((@($said | Where-Object { $_ -like '*version GOG*' }).Count -eq 0) -and $st -like '*choisi*') "firstrun: both games: Steam chosen (button: $st), no GOG help"
    # an exe the mod does not know (issue #5): which versions it needs, once, with the exe's fingerprint
    $bad = Join-Path $out "bad_game"; New-Item -ItemType Directory -Force $bad | Out-Null
    Set-Content "$bad\DDDA.exe" "not the game" -Encoding ascii
    $badSha = (Get-FileHash "$bad\DDDA.exe" -Algorithm SHA256).Hash
    $l = Start-Launcher "bad_exe" @("game=$bad") @('--steam', $empty, '--gog', $empty) 100
    $said = @(Read-Boxes $l 6)
    $said | ForEach-Object { "   unsupported exe showed: $($_.Substring(0, [Math]::Min(160, $_.Length)))..." }
    Stop-Process -Id $l.Id; Start-Sleep 1
    $checks += Check ($said.Count -eq 1 -and $said[0] -match 'build 2364871' -and $said[0] -match 'GOG' -and $said[0] -match $badSha) "firstrun: unsupported exe: one box naming the Steam build, GOG, and the exe's fingerprint"
    # the pages in the five languages, with the GOG game (the longest texts), and the GOG help box
    foreach ($lang in "FR", "EN", "DE", "IT", "ES") {
        $l = Start-Launcher "lang_$lang" @("lang=$lang", "game=J:\coop-test\gog1") @('--steam', $fake, '--gog', 'J:\coop-test\gog1', '--page', '3') 100
        Start-Sleep 1; Shot $l.MainWindowHandle "$out\lang_${lang}_settings.png"
        Cmd $l ($ID_NAV + 1); Start-Sleep 1; Shot $l.MainWindowHandle "$out\lang_${lang}_salon.png"
        Cmd $l ($ID_NAV + 0); Start-Sleep 1; Shot $l.MainWindowHandle "$out\lang_${lang}_play.png"
        [LW]::PostMessageW($l.MainWindowHandle, $WM_COMMAND, [IntPtr]130, [IntPtr]::Zero) | Out-Null
        Start-Sleep 1
        $dlg = [LW]::Dialog([uint32]$l.Id)
        if ($dlg -ne [IntPtr]::Zero) { Shot $dlg "$out\lang_${lang}_gog_help.png"; [LW]::PostMessageW($dlg, $WM_COMMAND, [IntPtr]2, [IntPtr]::Zero) | Out-Null }
        Stop-Process -Id $l.Id -ErrorAction SilentlyContinue; Start-Sleep 1
    }
    "   screenshots: $out\lang_*.png (looked at by hand)"
}

# ---- infolder: the mod unzipped into the game folder itself (issue #8: "Could not copy dinput8.dll to the
# game folder"): the launcher must go on, its dinput8.dll being the game's already ----
if (-not $Only -or $Only -eq "infolder") {
    $g4 = 'J:\coop-test\gog4'
    $added = @("DDDA-Coop.exe", "DDDA-Coop.ini", "dinput8.dll", "coop.ini") | Where-Object { -not (Test-Path (Join-Path $g4 $_)) }
    Copy-Item (Join-Path $build "DDDA-Coop.exe"), (Join-Path $build "dinput8.dll") $g4
    [IO.File]::WriteAllText("$g4\DDDA-Coop.ini", "[launcher]`r`nhelp_seen=1`r`ngog_help_seen=1`r`ncheck_updates=0`r`nlang=EN`r`nrole=host`r`ngame=$g4`r`n")
    New-Item -ItemType Directory -Force "$out\infolder_docs" | Out-Null
    $p = Start-Process "$g4\DDDA-Coop.exe" -ArgumentList @('--docs', "$out\infolder_docs", '--gog-storage', (Join-Path $out "fake_galaxy_storage"), '--no-launch') -PassThru
    for ($k = 0; $k -lt 40 -and $p.MainWindowHandle -eq 0; $k++) { Start-Sleep -Milliseconds 250; $p.Refresh() }
    Start-Sleep 1
    $said = @(Press-Launch $p)
    $said | ForEach-Object { "   infolder: launcher said: $_" }
    Stop-Process -Id $p.Id -ErrorAction SilentlyContinue; Start-Sleep 1
    $ini = Get-Content "$g4\coop.ini" -ErrorAction SilentlyContinue
    $checks += Check (-not ($said -match 'dinput8')) "infolder: no box about dinput8.dll when the launcher sits in the game folder"
    $checks += Check ($ini -contains "role=host") "infolder: coop.ini written for the host"
    foreach ($f in $added) { Remove-Item (Join-Path $g4 $f) -ErrorAction SilentlyContinue }
}

$checks += Check (-not (Compare-Object $before (Get-SaveHashes))) "owner's saves (Steam, GOG Galaxy) and game config unchanged"
$checks | ForEach-Object { "   $_" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
"launcher suite: $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
"outputs: $out"
if ($failed) { exit 1 }
