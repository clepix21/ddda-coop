# Join during play: the host loads its game and plays; the guest's game starts 140 s later. Its
# character reaches the host while the host is in game, so the host saves, loads the merged save and
# reloads its world (rejoin.cpp); the guest loads the host's save at its title (or reloads in game).
# Prints PASS/FAIL lines and a summary; exit 1 on any failure.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\join_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $mock = @("transport=steam", "steam_mock=1")
    $hostNet = $mock + @("mock_id=1", "mock_friends=2"); $guestNet = $mock + @("mock_id=2", "mock_friends=1")
}
$common = @("p2_pad=1", "idle_pad=3", "port=27101", "bridge=1")
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 360 -OutDir $out -Shots 90, 200, 300 -GuestSavesFromRole -GuestStartAt 140 `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "join_host.txt")) -GuestBridge @((Join-Path $tests "join_guest.txt")) `
    -HostIni ($common + @("role=host") + $hostNet) `
    -GuestIni ($common + @("role=guest", "guest_cam_pawn=1") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_join.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"join suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
