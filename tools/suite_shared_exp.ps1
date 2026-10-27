# Shared world exp: run "guest" (the guest, a real Arisen under another name, walks to the underground
# and fights while the host stays) and run "host" (the other way round). Prints PASS/FAIL lines.
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\shared_exp"
$fight = @((Join-Path $tests "route_guild_to_underground.txt"), (Join-Path $tests "exp_watch.txt"))
$hostIni = @("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "transport=udp", "shared_world=1", "bridge=1")
$guestIni = @("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "transport=udp", "peer=127.0.0.1", "shared_world=1", "bridge=1", "test_save=J:\coop-test\saves\guest_arisen.sav")
$bot = Join-Path $tests "bot_load_game_p2.txt"
$runs = @()
$runs += & (Join-Path $tools "run_pair.ps1") -Seconds 400 -OutDir "$out\guest" -Shots 300 -GuestSavesFromRole -HostBot $bot -GuestBot $bot `
    -GuestBridge $fight -HostIni $hostIni -GuestIni $guestIni 2>$null
$runs += & (Join-Path $tools "run_pair.ps1") -Seconds 400 -OutDir "$out\host" -Shots 300 -GuestSavesFromRole -HostBot $bot -GuestBot $bot `
    -HostBridge $fight -HostIni $hostIni -GuestIni $guestIni 2>$null
$checks = @(python -I (Join-Path $tools "check_shared_exp.py") $out)
if (($runs -match "saves and owner config unchanged: True").Count -eq 2) { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if (($runs -match "exited early True").Count -eq 0) { $checks += "PASS  all copies still running at the end of both runs" } else { $checks += "FAIL  all copies still running at the end of both runs" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"shared exp suite: $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
