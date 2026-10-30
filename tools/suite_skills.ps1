# Skills (shared world): the host draws its weapon and uses its weapon skills and attacks; every action
# both copies start is logged (test_log_actions=1), with screenshots. The guest's pawn showing the host
# must play each of them. Prints PASS/FAIL lines and a summary.
# -Mage: the host plays a sorcerer (guild_sorcerer.sav, tools/save_swap.py) and casts spells.
# -HostSave <file>: another caster for -Mage (guild_mage.sav: Marcus, mage, support spells).
# -HostScript <tests file>: the host's actions (skills_bow_host.txt with -Mage -HostSave ranger.sav: bows).
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Mage,
    [string]$HostSave = "J:\coop-test\saves\guild_sorcerer.sav", [string]$HostScript = "")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\skills_$Transport$(if ($Mage) { '_' + [IO.Path]::GetFileNameWithoutExtension($HostSave) })"
$hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
$shots = @(); for ($t = 104.0; $t -le 140; $t += 2) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 220 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests $(if ($HostScript) { $HostScript } elseif ($Mage) { "skills_mage_host.txt" } else { "skills_host.txt" }))) -GuestBridge @((Join-Path $tests "skills_guest.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "test_log_actions=1") + $(if ($Mage) { @("test_save=$HostSave") } else { @() }) + $hostNet) `
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
