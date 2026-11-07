# Cutscene: both copies load the owner's save before the Olra cutscene (saves/cutscene.sav, the
# Cassardis pier at night); the host waits 20 s (the guest is in game), then walks onto the pier.
# The guest's game must play the same cutscene at the same time. Prints PASS/FAIL lines and a summary.
# -Shared: the shared world (2026-11-07): the guest plays its own character and stands still; the host's
# start is sent to it (cutscene.cpp) and its game must play the same cutscene.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Shared)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\pair_cutscene_$Transport$(if ($Shared) { '_shared' })"
$mode = if ($Shared) { @("shared_world=1", "bridge=1") } else { @() }
$guestSave = if ($Shared) { "test_save=J:\coop-test\saves\guest_arisen.sav" } else { $save }
$save = "test_save=J:\coop-test\saves\cutscene.sav"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 190 -OutDir $out -Shots 126, 132, 150 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "cutscene_host.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", $save) + $mode + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "guest_cam_pawn=1", $guestSave) + $mode + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_cutscene.py") $out)
if ($Shared) { $checks = @($checks | Where-Object { $_ -notmatch "follows the host" }) } # its own character there
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"cutscene suite ($Transport$(if ($Shared) { ', shared' })): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
