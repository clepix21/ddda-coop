# Guest progress: (1) two copies; the guest's own save is guest_lv44.sav (its Arisen is a level-44
# character), which plays hired pawn 1 on the host; the host walks to the underground, its party kills
# the small creatures and it saves (mode 7). The guest must keep the party's gains. (2) The guest's copy
# alone (no role, keep_progress=1) loads its own save: the gains are added (backup first) and its next
# save holds them. Prints PASS/FAIL lines and a summary.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\progress_$Transport"
$guestDir = "J:\coop-test\DDDA2"
$own = "J:\coop-test\saves\guest_lv44.sav"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
foreach ($d in "J:\coop-test\DDDA", $guestDir) {
    Remove-Item (Join-Path $d "coop_progress") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $d "write_*.sav") -ErrorAction SilentlyContinue
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 420 -OutDir $out -Shots 410 `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "route_guild_to_underground.txt"), (Join-Path $tests "progress_host.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "test_save=$own") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$solo = & (Join-Path $tools "run_test.ps1") -GameDir $guestDir -Seconds 130 -Bot (Join-Path $tests "bot_load_game_p2.txt") `
    -BridgeScripts (Join-Path $tests "progress_apply.txt") -Shots 120 -OutDir (Join-Path $out "solo") `
    -Ini @("p2_pad=1", "idle_pad=3", "bridge=1", "dump_writes=1", "keep_progress=1", "test_save=$own") 2>$null
$solo | ForEach-Object { "   solo: $_" }
Copy-Item (Join-Path $out "solo\coop.log") (Join-Path $out "solo.log") -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out "solo.log"))) { New-Item (Join-Path $out "solo.log") | Out-Null }
$checks = @(python -I (Join-Path $tools "check_progress.py") $out $guestDir $own)
if ($run -match "saves and owner config unchanged: True" -and $solo -match "saves unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True" -or $solo -match "game exited early: True") { $checks += "FAIL  copies still running at the end" } else { $checks += "PASS  copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"progress suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
