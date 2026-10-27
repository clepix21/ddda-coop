# Soak, three copies over the Steam path (test stand-in): the host walks from Gran Soren through the
# iron gate into the underground and its party fights there; guest 1's game is killed at 200 s and
# started again 15 s later; 8 minutes in all. Nothing may crash or stall, guest 1 must come back
# with its character (no world reload: the same character), and guest 2, never interrupted, keeps
# following the host. Prints PASS/FAIL lines and a summary; exit 1 on any failure.
param([int]$Loss = 0, [int]$Latency = 0)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\soak"
Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
$mock = @("transport=steam", "steam_mock=1", "mock_loss=$Loss", "mock_latency=$Latency")
$common = @("p2_pad=1", "idle_pad=3", "port=27101", "bridge=1")
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 480 -OutDir $out -Shots 150, 300, 450 -GuestSavesFromRole `
    -RestartGuestAt 200 -RestartDelay 15 `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -Guest2Bot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "route_guild_to_underground.txt"), (Join-Path $tests "soak_host.txt")) `
    -GuestBridge @((Join-Path $tests "party_guest.txt")) -Guest2Bridge @((Join-Path $tests "party_guest.txt")) `
    -HostIni ($common + @("role=host", "mock_id=1", "mock_friends=2,3") + $mock) `
    -GuestIni ($common + @("role=guest", "guest_cam_pawn=1", "mock_id=2", "mock_friends=1") + $mock) `
    -Guest2Ini ($common + @("role=guest", "guest_cam_pawn=1", "mock_id=3", "mock_friends=1") + $mock) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_soak.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  all copies still running at the end" } else { $checks += "PASS  all copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"soak suite (mock, loss $Loss%, latency $Latency ms): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
