# Shared world (shared_world=1), two copies: the guest (its own save: a real Arisen under another name) plays its
# own character in its copy of the host's world; both see each other; the guest walks on its own; the
# host goes through the iron gate to the underground and the guest stays. Prints PASS/FAIL lines.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\shared_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 260 -OutDir $out -Shots 120, 170, 230 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "shared_guest_walk.txt") `
    -HostBridge @((Join-Path $tests "talk_wait.txt"), (Join-Path $tests "route_guild_to_underground.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "shared_world=1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_shared.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"shared suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
