# Monster lifts (shared world): both copies go to the underground; the host lifts the nearest small
# enemy with RT three times, the guest stays next to the pawn showing the host. On the guest that pawn
# must hold the monster (the game's own catch, or carried where the host has it). PASS/FAIL and a summary.
# -Pose: the guest's carrying pawn also plays its player's lift action (test_carry_pose=1).
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Pose)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\lift_$Transport"
$route = Join-Path $tests "route_guild_to_underground.txt"
$shots = @(); for ($t = 150.0; $t -le 230; $t += 4) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 260 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @($route, (Join-Path $tests "lift_host.txt")) -GuestBridge @($route, (Join-Path $tests "lift_guest.txt")) `
    -HostIni @("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "transport=udp") `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "transport=udp", "peer=127.0.0.1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $(if ($Pose) { @("test_carry_pose=1") } else { @() })) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_lift.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"lift suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
