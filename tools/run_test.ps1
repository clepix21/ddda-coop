# Runs the test copy with the freshly built DLL for N seconds, then kills it and checks that the
# owner's save folder is byte-identical. Exit 0 = run ok and saves unchanged.
# Usage: .\run_test.ps1 -Seconds 45 [-Bot script.txt] [-Shots 10,20] [-OutDir dir]
#        [-SaveDir <steam userdata\<id>\367500>] [-GameDir J:\coop-test\DDDA]
param(
    [int]$Seconds = 45,
    [string]$Bot = "",
    [double[]]$Shots = @(),
    [string[]]$Keys = @(),   # "start_s:scancode_hex:duration_s", e.g. "140:11:3" holds W for 3 s
    [string[]]$Ini = @("p2_pad=1", "idle_pad=3"),  # extra coop.ini lines; block_saves/debug_log are forced on
    [string[]]$BridgeScripts = @(),  # files appended to bridge_in.txt once the Arisen is in game (needs bridge=1)
    [string]$OutDir = "J:\coop-test\re\runs\latest",
    [string]$GameDir = "J:\coop-test\DDDA",
    # the game's Steam Cloud folder (the first account on this PC that has one), checked unchanged
    [string]$SaveDir = (Get-ChildItem "${env:ProgramFiles(x86)}\Steam\userdata\*\367500" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1).FullName,
    [string]$Dll = "$PSScriptRoot\..\build\dinput8.dll"
)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Coop -Name Kbd -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
$keyEvents = foreach ($k in $Keys) { $f = $k.Split(':'); [pscustomobject]@{ Start = [double]$f[0]; Scan = [Convert]::ToByte($f[1], 16); End = [double]$f[0] + [double]$f[2]; Down = $false; Done = $false } }
$OwnerConfig = "$env:LOCALAPPDATA\CAPCOM\DRAGONS DOGMA DARK ARISEN\config.ini"
function Get-SaveHashes {
    Get-ChildItem $SaveDir -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName)|$((Get-FileHash $_.FullName).Hash)" }
    "$OwnerConfig|$((Get-FileHash $OwnerConfig).Hash)"   # the owner's game settings must not change either
}
function Save-Shot([string]$path) {
    $r = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $r.Width, $r.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Location, [System.Drawing.Point]::Empty, $r.Size)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($r.Width / 2)), ([int]($r.Height / 2))
    $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -File | Remove-Item
$stopFile = Join-Path $OutDir "STOP"   # create this file to end a long (bridge) session early
Copy-Item $Dll $GameDir -Force
# Tests never write saves: the save guard is forced on, and the run is aborted if it is not active in 20 s.
# the game reads a frozen copy of a save (test_save, default: the Pawn Guild one every route starts
# from), never the owner's current one, which moves when they play
$testSave = if (($Ini -join "`n") -match "(?m)^test_save=") { @() } else { @("test_save=J:\coop-test\saves\guild.sav") }
$classic = if (($Ini -join "`n") -match "(?m)^shared_world=") { @() } else { @("shared_world=0") } # the shared world is the default since 2026-11-06
(@("block_saves=1", "debug_log=1", "private_appdata=1") + $classic + $testSave + $Ini) | Set-Content (Join-Path $GameDir "coop.ini") -Encoding ascii
# private game config (copied read-only from the owner's), so the owner's settings file is never rewritten
$cfgDir = Join-Path $GameDir "coop_appdata\CAPCOM\DRAGONS DOGMA DARK ARISEN"
New-Item -ItemType Directory -Force $cfgDir | Out-Null
Copy-Item $OwnerConfig (Join-Path $cfgDir "config.ini") -Force
$botPath = Join-Path $GameDir "bot.txt"
if ($Bot) { Copy-Item $Bot $botPath -Force } elseif (Test-Path $botPath) { Remove-Item $botPath }
$before = Get-SaveHashes
$start = Get-Date
$p = Start-Process (Join-Path $GameDir "DDDA.exe") -WorkingDirectory $GameDir -PassThru
$log = Join-Path $GameDir "coop.log"
$guardOk = $false
$bridgeSent = $BridgeScripts.Count -eq 0
$bridgeOut = Join-Path $GameDir "bridge_out.txt"
Remove-Item $bridgeOut -ErrorAction SilentlyContinue
$pending = [System.Collections.Generic.List[double]]::new(); $Shots | ForEach-Object { $pending.Add($_) }
# COOP_SHOT_EVERY=<s> (environment): also a shot every <s> seconds once in game (from 90 s), for screenshot reviews
if ($env:COOP_SHOT_EVERY) { for ($t = 90.0; $t -lt $Seconds; $t += [double]$env:COOP_SHOT_EVERY) { if (-not $pending.Contains($t)) { $pending.Add($t) } } }
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 250
    if (Test-Path $stopFile) { "STOP file found"; break }
    if (-not $bridgeSent -and (Test-Path $bridgeOut) -and (Select-String -Path $bridgeOut -Pattern "arisen ok" -Quiet)) {
        $BridgeScripts | ForEach-Object { Get-Content $_ | Where-Object { $_ -and -not $_.StartsWith("#") } } | Add-Content (Join-Path $GameDir "bridge_in.txt") -Encoding ascii
        "bridge scripts sent at $([int]((Get-Date) - $start).TotalSeconds) s"
        $bridgeSent = $true
    }
    if (-not $guardOk -and (Test-Path $log) -and (Select-String -Path $log -Pattern "save-guard: ACTIVE" -Quiet)) { $guardOk = $true }
    if (-not $guardOk -and ((Get-Date) - $start).TotalSeconds -gt 20) { "save guard not active after 20 s: killing the game"; break }
    $el = ((Get-Date) - $start).TotalSeconds
    foreach ($k in @($keyEvents)) {
        if (-not $k.Down -and -not $k.Done -and $el -ge $k.Start) { [Coop.Kbd]::keybd_event(0, $k.Scan, 0x8, [UIntPtr]::Zero); $k.Down = $true }
        if ($k.Down -and $el -ge $k.End) { [Coop.Kbd]::keybd_event(0, $k.Scan, 0xA, [UIntPtr]::Zero); $k.Down = $false; $k.Done = $true }
    }
    foreach ($s in @($pending)) { if ($el -ge $s) { Save-Shot (Join-Path $OutDir ("shot_{0:000.0}s.png" -f $s)); [void]$pending.Remove($s) } }
}
$exitedEarly = $p.HasExited
try { $conns = Get-NetTCPConnection -OwningProcess $p.Id -ErrorAction Stop | Where-Object { $_.RemoteAddress -notin @('127.0.0.1','::1','0.0.0.0','::') } } catch { $conns = @() }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null }
Copy-Item $log (Join-Path $OutDir "coop.log") -ErrorAction SilentlyContinue
$after = Get-SaveHashes
$savesOk = -not (Compare-Object $before $after)
"guard active: $guardOk"
"saves unchanged: $savesOk"
"game exited early: $exitedEarly"
"non-local TCP connections of DDDA.exe at end: $(@($conns).Count) $(@($conns | ForEach-Object { "$($_.RemoteAddress):$($_.RemotePort)/$($_.State)" }) -join ' ')"
"outputs: $OutDir"
if (-not $savesOk) { "SAVE FILES CHANGED - restore from J:\coop-test\save-backup"; exit 2 }
exit 0
