# 2 to 4 players, step 2 (three copies): each guest's own character replaces the pawn it drives.
# Both guests connect while the host is at the title; the host merges their characters (from their
# own saves; in tests the frozen guild save, Arisen "Clement") over hired pawns 1 and 2, its game and
# both guests load that save. Then the host goes through the iron gate: its autosave must be
# un-merged. Prints PASS/FAIL lines and a summary; exit 1 on any failure.
# -Four: a third guest (fourth copy) takes the main pawn: all three pawns are friends' characters.
# -Look: guest 1 plays another character (guest_arisen.sav, Arisen "Invite"): every copy must show it on pawn 1.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Four, [switch]$Look)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\party_$Transport$(if ($Four) { '_four' })$(if ($Look) { '_look' })"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $g1Net = @("transport=udp", "peer=127.0.0.1"); $g2Net = $g1Net; $g3Net = $g1Net
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $mock = @("transport=steam", "steam_mock=1")
    $hostNet = $mock + @("mock_id=1", "mock_friends=2,3,4")
    $g1Net = $mock + @("mock_id=2", "mock_friends=1"); $g2Net = $mock + @("mock_id=3", "mock_friends=1")
    $g3Net = $mock + @("mock_id=4", "mock_friends=1")
}
$common = @("p2_pad=1", "idle_pad=3", "port=27101", "bridge=1")
$g1Save = if ($Look) { @("test_save=J:\coop-test\saves\guest_arisen.sav") } else { @() }
$extra = @{}
if ($Four) {
    $extra = @{ Guest3Bot = (Join-Path $tests "bot_load_game_p2.txt"); Guest3Bridge = @((Join-Path $tests "party_guest.txt"))
                Guest3Ini = ($common + @("role=guest", "guest_cam_pawn=1") + $g3Net) }
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 270 -OutDir $out -Shots 120, 140 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -Guest2Bot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "party_host.txt"), (Join-Path $tests "route_guild_to_underground.txt")) `
    -GuestBridge @((Join-Path $tests "party_guest.txt")) -Guest2Bridge @((Join-Path $tests "party_guest.txt")) `
    -HostIni ($common + @("role=host") + $hostNet) `
    -GuestIni ($common + @("role=guest", "guest_cam_pawn=1") + $g1Net + $g1Save) `
    -Guest2Ini ($common + @("role=guest", "guest_cam_pawn=1") + $g2Net) @extra 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_party.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  all copies still running at the end" } else { $checks += "PASS  all copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"party suite ($Transport$(if ($Four) { ', four' })): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
