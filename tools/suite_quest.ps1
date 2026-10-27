# Shared world quests (owner's quest.sav, Cassardis, a quest giver in front): run "host", the host
# talks (B) and accepts with A, the guest's world must get the quest; run "guest", the other way round.
# Each run ends with a save request on the other side (dumped) to read its quest list.
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\quest"
$save = "test_save=J:\coop-test\saves\quest.sav"
$take = Join-Path $tests "quest_take_host.txt"
$stay = Join-Path $tests "quest_take_guest.txt"
$bot = Join-Path $tests "bot_load_game_p2.txt"
$hostIni = @("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "transport=udp", "bridge=1", "shared_world=1", "dump_writes=1", $save)
$guestIni = @("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "transport=udp", "peer=127.0.0.1", "bridge=1", "shared_world=1", "dump_writes=1", "test_save=J:\coop-test\saves\guest_arisen.sav")
$runs = @()
foreach ($r in @(@{Name = "host"; HostB = $take; GuestB = $stay; Other = "J:\coop-test\DDDA2"; OtherName = "guest"},
                 @{Name = "guest"; HostB = $stay; GuestB = $take; Other = "J:\coop-test\DDDA"; OtherName = "host"})) {
    foreach ($d in "J:\coop-test\DDDA", "J:\coop-test\DDDA2") { Remove-Item (Join-Path $d "write_*.sav") -ErrorAction SilentlyContinue }
    $runs += & (Join-Path $tools "run_pair.ps1") -Seconds 260 -OutDir "$out\$($r.Name)" -Shots 200 -GuestSavesFromRole -HostBot $bot -GuestBot $bot `
        -HostBridge @($r.HostB) -GuestBridge @($r.GuestB) -HostIni $hostIni -GuestIni $guestIni 2>$null
    $last = Get-ChildItem (Join-Path $r.Other "write_*.sav") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($last) { Copy-Item $last.FullName "$out\$($r.Name)\$($r.OtherName)_last.sav" -Force }
}
$checks = @(python -I (Join-Path $tools "check_quest.py") $out)
if (($runs -match "saves and owner config unchanged: True").Count -eq 2) { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if (($runs -match "exited early True").Count -eq 0) { $checks += "PASS  all copies still running at the end of both runs" } else { $checks += "FAIL  all copies still running at the end of both runs" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"quest suite: $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
