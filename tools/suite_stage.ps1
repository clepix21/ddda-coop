# Two-copy stage change (milestone 5): both copies load the save and connect; the host's bot walks
# to the iron gate and goes through it (stage 220 -> 600, a load and an autosave the guard blocks),
# then on into the underground. The guest's game must follow to the same stage, and its Arisen must
# follow the host's there. The guest runs with block_saves=0: its role alone must block its saves
# (the autosave of the area change). Prints PASS/FAIL lines and a summary; exit 1 on any failure.
# -LateGuest: the guest connects only after the host is in the new stage (late join), so it must
# catch up through the 10 s "stages differ" rule.
# -Scenario death: after the gate the host's Arisen dies (HP set to 1 near the creatures) and the
# bot picks "Réessayer" on the game-over screen: a reload that is not a jump; the guest must reload too.
# -Scenario title: the bot picks "Retour à l'écran titre" instead and loads the game again (stage 220).
# -Scenario lastsave: the bot picks "Dernier point de sauvegarde".
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$LateGuest,
      [ValidateSet("gate", "death", "title", "lastsave")][string]$Scenario = "gate")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\pair_stage_$Transport$(if ($LateGuest) { '_late' })$(if ($Scenario -ne 'gate') { "_$Scenario" })"
$hostBridge = @((Join-Path $tests "route_guild_to_underground.txt"))
if ($Scenario -eq "death") { $hostBridge += (Join-Path $tests "death_reload.txt") }
if ($Scenario -eq "title") { $hostBridge += (Join-Path $tests "death_title_load.txt") }
if ($Scenario -eq "lastsave") { $hostBridge += (Join-Path $tests "death_last_save.txt") }
$seconds = @{ gate = 290; death = 330; title = 360; lastsave = 360 }[$Scenario]
$late = if ($LateGuest) { @("bridge=1", "net_late=1") } else { @() }
# death: the guest keeps its character home, so the host's party is its own (a guest's Arisen-level
# character as pawn 1 killed every creature before the Arisen at 1 HP was hit, 2026-10-27 03:51, 04:03)
if ($Scenario -ne "gate") { $late += @("send_character=0") }
$guestBridge = if ($LateGuest) { @((Join-Path $tests "stage_late_guest.txt")) } else { @() }
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds $seconds -OutDir $out -Shots 200, 280, 320 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge $hostBridge -GuestBridge $guestBridge `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "guest_cam_pawn=1") + $late + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_stage.py") $out $(if ($Scenario -ne "gate") { "--reload" }))
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"stage suite ($Transport$(if ($LateGuest) { ', late guest' })$(if ($Scenario -ne 'gate') { ", $Scenario" })): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
