# Guest hits (shared world, issue #10): both copies go to the underground; the guest walks to the nearest
# small enemy and hits it with X, three rounds; the host's Arisen does not attack. The guest's own hits must
# be sent to the host and applied there (MSG_HIT), and the pawn showing the guest must deal no damage of its
# own meanwhile. PASS/FAIL and a summary.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\guest_hits_$Transport"
$route = Join-Path $tests "route_guild_to_underground.txt"
$shots = @(); for ($t = 150.0; $t -le 230; $t += 4) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 260 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @($route, (Join-Path $tests "guest_hits_host.txt")) -GuestBridge @($route, (Join-Path $tests "guest_hits_guest.txt")) `
    -HostIni @("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "transport=udp") `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "transport=udp", "peer=127.0.0.1", "test_save=J:\coop-test\saves\guest_arisen.sav")) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_guest_hits.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"guest hits suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
