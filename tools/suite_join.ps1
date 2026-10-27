# Join during play: the host loads its game and plays; the guest's game starts 140 s later. Its
# character reaches the host while the host is in game, so the host saves, loads the merged save and
# reloads its world (rejoin.cpp); the guest loads the host's save at its title (or reloads in game).
# -Three: three copies. Guest 1 starts with the host (its character merged at the title); guest 2
# starts 160 s later, so the host reloads during play AND guest 1, already in game, reloads the
# host's new save (pawns 1 and 2 then have the guests' characters everywhere).
# Prints PASS/FAIL lines and a summary; exit 1 on any failure.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Three)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\join_$Transport$(if ($Three) { '_three' })"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1"); $guest2Net = $guestNet
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $mock = @("transport=steam", "steam_mock=1")
    $hostNet = $mock + @("mock_id=1", "mock_friends=2,3"); $guestNet = $mock + @("mock_id=2", "mock_friends=1")
    $guest2Net = $mock + @("mock_id=3", "mock_friends=1")
}
$common = @("p2_pad=1", "idle_pad=3", "port=27101", "bridge=1")
$guestIni = $common + @("role=guest", "guest_cam_pawn=1")
if ($Three) {
    $extra = @{ Guest2StartAt = 160; Guest2Bot = (Join-Path $tests "bot_load_game_p2.txt")
                Guest2Bridge = @((Join-Path $tests "join_guest.txt")); Guest2Ini = ($guestIni + $guest2Net) }
} else {
    $extra = @{ GuestStartAt = 140 }
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds $(if ($Three) { 380 } else { 360 }) -OutDir $out -Shots 90, 200, 300 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "join_host.txt")) -GuestBridge @((Join-Path $tests "join_guest.txt")) `
    -HostIni ($common + @("role=host") + $hostNet) -GuestIni ($guestIni + $guestNet) @extra 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_join.py") $out $(if ($Three) { "--three" }))
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  all copies still running at the end" } else { $checks += "PASS  all copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"join suite ($Transport$(if ($Three) { ', three' })): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
