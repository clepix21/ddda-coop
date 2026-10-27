# Two-copy enemy sync (milestone 4): both copies load the save and walk to the underground on
# their own (net_late=1, so the network starts only once both are there), then the host's Arisen
# walks up to the small creatures and the party fights. The guest must pair its enemies with the
# host's, follow their positions and end with the same HP. Prints PASS/FAIL lines and a summary.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\pair_enemies_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
foreach ($d in "J:\coop-test\DDDA", "J:\coop-test\DDDA2") { Remove-Item (Join-Path $d "dump_end_*") -ErrorAction SilentlyContinue }
$route = Join-Path $tests "route_guild_to_underground.txt"
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 350 -OutDir $out -Shots 240, 330 `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @($route, (Join-Path $tests "pair_enemy_host.txt")) -GuestBridge @($route, (Join-Path $tests "pair_enemy_guest.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "net_late=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "net_late=1") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
Copy-Item "J:\coop-test\DDDA\dump_end_enemies.txt" (Join-Path $out "host_enemies.txt") -ErrorAction SilentlyContinue
Copy-Item "J:\coop-test\DDDA2\dump_end_enemies.txt" (Join-Path $out "guest_enemies.txt") -ErrorAction SilentlyContinue
foreach ($f in "host_enemies.txt", "guest_enemies.txt") { if (-not (Test-Path (Join-Path $out $f))) { New-Item (Join-Path $out $f) | Out-Null } }
$checks = @(python -I (Join-Path $tools "check_enemies.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"enemy suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
