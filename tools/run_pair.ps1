# Runs two copies of the test install side by side (host = DDDA, guest = DDDA2), each with its
# own bot script and coop.ini lines, for N seconds or until <OutDir>\STOP exists. Checks that the
# owner's save folder is byte-identical afterwards. Tests only: forces block_saves, debug_log
# and multi_instance on both copies.
# Usage: .\run_pair.ps1 -Seconds 200 -HostBot a.txt -GuestBot b.txt [-HostIni ...] [-GuestIni ...]
param(
    [int]$Seconds = 200,
    [string]$HostBot = "",
    [string]$GuestBot = "",
    [string[]]$HostIni = @(),
    [string[]]$GuestIni = @(),
    [string[]]$HostBridge = @(),
    [string[]]$GuestBridge = @(),
    [double[]]$Shots = @(),
    [string]$OutDir = "J:\coop-test\re\runs\pair",
    # the game's Steam Cloud folder (the first account on this PC that has one), checked unchanged
    [string]$SaveDir = (Get-ChildItem "${env:ProgramFiles(x86)}\Steam\userdata\*\367500" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1).FullName,
    [string]$OwnerConfig = "$env:LOCALAPPDATA\CAPCOM\DRAGONS DOGMA DARK ARISEN\config.ini",
    [string]$HostWindow = "0,0,1920,1080",       # middle screen (primary)
    [string]$GuestWindow = "-1920,0,1920,1080",  # left screen
    [string]$Dll = "$PSScriptRoot\..\build\dinput8.dll",
    [switch]$AllowRealSteam,
    [double]$RestartGuestAt = 0,   # seconds; 0 = never
    [double]$RestartDelay = 15,
    # the guest copy runs with block_saves=0, so only its role (guest = saves always blocked) keeps
    # its saves off; the save guard must still report ACTIVE within 20 s or the run stops
    [switch]$GuestSavesFromRole,
    # a second guest (third copy, DDDA3): both guests then share the left screen, 960x540 each
    [string]$Guest2Bot = "",
    [string[]]$Guest2Ini = @(),
    [string[]]$Guest2Bridge = @(),
    # the (first) guest's game starts that many seconds after the host's (a friend joining during play)
    [double]$GuestStartAt = 0,
    [double]$Guest2StartAt = 0,
    # a third guest (fourth copy, DDDA4), top right quarter of the left screen
    [string]$Guest3Bot = "",
    [string[]]$Guest3Ini = @(),
    [string[]]$Guest3Bridge = @(),
    [double]$Guest3StartAt = 0
)
function Get-SaveHashes {
    Get-ChildItem $SaveDir -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName)|$((Get-FileHash $_.FullName).Hash)" }
    "$OwnerConfig|$((Get-FileHash $OwnerConfig).Hash)"   # the owner's game settings must not change either
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -File | Remove-Item
$stopFile = Join-Path $OutDir "STOP"
$sides = @(
    @{ Name = "host"; Dir = "J:\coop-test\DDDA"; Bot = $HostBot; Ini = $HostIni; Bridge = $HostBridge; Window = $HostWindow; StartAt = 0 },
    @{ Name = "guest"; Dir = "J:\coop-test\DDDA2"; Bot = $GuestBot; Ini = $GuestIni; Bridge = $GuestBridge; Window = $GuestWindow; StartAt = $GuestStartAt }
)
if ($Guest2Ini.Count -gt 0) {
    $sides[1].Window = "-1920,0,960,540"
    $sides += @{ Name = "guest2"; Dir = "J:\coop-test\DDDA3"; Bot = $Guest2Bot; Ini = $Guest2Ini; Bridge = $Guest2Bridge; Window = "-1920,540,960,540"; StartAt = $Guest2StartAt }
}
if ($Guest3Ini.Count -gt 0) {
    $sides += @{ Name = "guest3"; Dir = "J:\coop-test\DDDA4"; Bot = $Guest3Bot; Ini = $Guest3Ini; Bridge = $Guest3Bridge; Window = "-960,0,960,540"; StartAt = $Guest3StartAt }
}
# Real Steam (rich presence on the owner's account, P2P to friends) only when asked for explicitly.
foreach ($s in $sides) {
    $ini = $s.Ini -join "`n"
    if ($ini -match "(?m)^transport=steam" -and $ini -notmatch "(?m)^steam_mock=1" -and -not $AllowRealSteam) {
        throw "$($s.Name): transport=steam without steam_mock=1 would use the owner's real Steam account; pass -AllowRealSteam"
    }
    if ($ini -notmatch "(?m)^transport=") { $s.Ini = @("transport=udp") + $s.Ini }
}
function Start-Side($s, $el) {
    $d = $s.Dir
    # private files: delete first so a hard link is never written through
    foreach ($f in "dinput8.dll", "coop.ini", "bot.txt", "coop.log", "bridge_in.txt", "bridge_out.txt") { Remove-Item (Join-Path $d $f) -ErrorAction SilentlyContinue }
    Copy-Item $Dll $d
    $block = if ($GuestSavesFromRole -and $s.Name -like "guest*") { "block_saves=0" } else { "block_saves=1" }
    # a frozen copy of a save (default: the Pawn Guild one every route starts from), never the owner's
    $testSave = if (($s.Ini -join "`n") -match "(?m)^test_save=") { @() } else { @("test_save=J:\coop-test\saves\guild.sav") }
    (@($block, "debug_log=1", "multi_instance=1", "keep_active=1", "private_appdata=1", "window=$($s.Window)") + $testSave + $s.Ini) | Set-Content (Join-Path $d "coop.ini") -Encoding ascii
    # private game config (copied read-only from the owner's, windowed), so the owner's file is never rewritten
    $cfgDir = Join-Path $d "coop_appdata\CAPCOM\DRAGONS DOGMA DARK ARISEN"
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    (Get-Content $OwnerConfig) -replace '^FullScreen=ON$', 'FullScreen=OFF' | Set-Content (Join-Path $cfgDir "config.ini") -Encoding ascii
    if ($s.Bot) { Copy-Item $s.Bot (Join-Path $d "bot.txt") }
    $s.Proc = Start-Process (Join-Path $d "DDDA.exe") -WorkingDirectory $d -PassThru
    $s.Guard = $false; $s.BridgeSent = ($s.Bridge.Count -eq 0); $s.Launched = $el
}
$before = Get-SaveHashes
$start = Get-Date
foreach ($s in $sides) {
    if ($s.StartAt -gt 0) { $s.Proc = $null; continue }
    Start-Side $s ((Get-Date) - $start).TotalSeconds; Start-Sleep 5
}
$guestSide = $sides[1]; $restartAt = 0
$pending = [System.Collections.Generic.List[double]]::new(); $Shots | ForEach-Object { $pending.Add($_) }
# COOP_SHOT_EVERY=<s> (environment): also a shot every <s> seconds once in game (from 90 s), for screenshot reviews
if ($env:COOP_SHOT_EVERY) { for ($t = 90.0; $t -lt $Seconds; $t += [double]$env:COOP_SHOT_EVERY) { if (-not $pending.Contains($t)) { $pending.Add($t) } } }
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 250
    if (Test-Path $stopFile) { "STOP file found"; break }
    $el = ((Get-Date) - $start).TotalSeconds
    foreach ($s in $sides) { if ($s.StartAt -gt 0 -and -not $s.Proc -and $el -ge $s.StartAt) { Start-Side $s $el; "$($s.Name) started at $([int]$el) s" } }
    foreach ($s in $sides) {
        if (-not $s.Proc) { continue }
        $log = Join-Path $s.Dir "coop.log"
        if (-not $s.Guard -and (Test-Path $log) -and (Select-String -Path $log -Pattern "save-guard: ACTIVE" -Quiet)) { $s.Guard = $true }
        if (-not $s.Guard -and ($el - $s.Launched) -gt 25) { "$($s.Name): save guard not active after 20 s, stopping"; $Seconds = 0 }
        $bo = Join-Path $s.Dir "bridge_out.txt"
        if (-not $s.BridgeSent -and (Test-Path $bo) -and (Select-String -Path $bo -Pattern "arisen ok" -Quiet)) {
            $s.Bridge | ForEach-Object { Get-Content $_ | Where-Object { $_ -and -not $_.StartsWith("#") } } | Add-Content (Join-Path $s.Dir "bridge_in.txt") -Encoding ascii
            "$($s.Name): bridge scripts sent at $([int]$el) s"; $s.BridgeSent = $true
        }
    }
    foreach ($t in @($pending)) { if ($el -ge $t) { & (Join-Path $PSScriptRoot "shot2.ps1") (Join-Path $OutDir ("shot_{0:000.0}s.png" -f $t)); [void]$pending.Remove($t) } }
    # -RestartGuestAt: the guest's game is killed (a crash or a dropped friend), then started again
    if ($RestartGuestAt -gt 0 -and $el -ge $RestartGuestAt -and -not $restartAt) {
        Stop-Process -Id $guestSide.Proc.Id -Force; $guestSide.Proc.WaitForExit(10000) | Out-Null
        Copy-Item (Join-Path $guestSide.Dir "coop.log") (Join-Path $OutDir "guest_first.log") -ErrorAction SilentlyContinue
        "guest killed at $([int]$el) s"; $restartAt = $el + $RestartDelay
    }
    if ($restartAt -gt 0 -and $el -ge $restartAt) { Start-Side $guestSide $el; "guest started again at $([int]$el) s"; $restartAt = -1 }
    if (($sides | Where-Object { $_.Proc -and $_.Proc.HasExited }).Count -and $restartAt -le 0) { "a copy exited early"; break }
}
foreach ($s in $sides) {
    if (-not $s.Proc) { "$($s.Name): never started"; continue }
    $exited = $s.Proc.HasExited
    if (-not $exited) { Stop-Process -Id $s.Proc.Id -Force; $s.Proc.WaitForExit(10000) | Out-Null }
    Copy-Item (Join-Path $s.Dir "coop.log") (Join-Path $OutDir "$($s.Name).log") -ErrorAction SilentlyContinue
    "$($s.Name): guard active $($s.Guard), exited early $exited"
}
$savesOk = -not (Compare-Object $before (Get-SaveHashes))
"saves and owner config unchanged: $savesOk"
"outputs: $OutDir"
if (-not $savesOk) { "SAVE FILES CHANGED - restore from J:\coop-test\save-backup"; exit 2 }
exit 0
