# Skills (shared world): the host draws its weapon and uses its weapon skills and attacks; every action
# both copies start is logged (test_log_actions=1), with screenshots. The guest's pawn showing the host
# must play each of them. Prints PASS/FAIL lines and a summary.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\skills_$Transport"
$hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
$shots = @(); for ($t = 104.0; $t -le 140; $t += 2) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 220 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "skills_host.txt")) -GuestBridge @((Join-Path $tests "skills_guest.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "test_log_actions=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "test_log_actions=1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_skills.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"skills suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
