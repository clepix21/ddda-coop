# Issue #4 (desynced enemies on the road out of Cassardis): shared world, two copies in stage 100
# (saves/ocean.sav, on the coast). The host walks west to the goblins and fights them, the guest walks
# the same way a little behind. The guest must pair its enemies with the host's and keep them where the
# host's stand. Prints PASS/FAIL lines and a summary.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [string[]]$ExtraIni = @())
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\cassardis_$Transport"
$copy1 = if ($env:COOP_GOG -eq "1") { "J:\coop-test\gog1" } else { "J:\coop-test\DDDA" }
$copy2 = if ($env:COOP_GOG -eq "1") { "J:\coop-test\gog2" } else { "J:\coop-test\DDDA2" }
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
foreach ($d in $copy1, $copy2) { Get-ChildItem $d -Filter "dump_*" -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName } }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 330 -OutDir $out -Shots 150, 220, 300 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "cassardis_host.txt")) -GuestBridge @((Join-Path $tests "cassardis_guest.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "test_save=J:\coop-test\saves\ocean.sav") + $hostNet + $ExtraIni) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $guestNet + $ExtraIni) 2>$null
$run | ForEach-Object { "   $_" }
Copy-Item "$copy1\dump_end_enemies.txt" (Join-Path $out "host_enemies.txt") -ErrorAction SilentlyContinue
Copy-Item "$copy2\dump_end_enemies.txt" (Join-Path $out "guest_enemies.txt") -ErrorAction SilentlyContinue
foreach ($f in "host_enemies.txt", "guest_enemies.txt") { if (-not (Test-Path (Join-Path $out $f))) { New-Item (Join-Path $out $f) | Out-Null } }
$checks = @(python -I (Join-Path $tools "check_cassardis.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"cassardis suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
