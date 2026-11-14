# Grabs between players (shared world; issue #1: "When I capture a friend, his screen teleports and after
# his throw everything returns to normal, he cannot grab me"). Pawn Guild: the host grabs the guest twice
# (RT on the pawn showing it), then the guest grabs the host twice. Both copies log the Arisen's and the
# pawns' places every 100 ms (test_log_heights=1); check_player_grab.py reads the holds and the jumps.
param([ValidateSet("udp", "mock")][string]$Transport = "udp",
      # -Rough: kicks and a walk while holding, the held one struggles (Nexus 2026-11-08: "the camera goes underground",
      # "the guest turns into a frog on a journey of endless jumps"); -HostSave: another place (outdoors)
      [switch]$Rough, [string]$HostSave = "", [string[]]$ExtraIni = @())
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$tag = if ($Rough) { "rough_" } else { "" }
if ($HostSave) { $tag += [IO.Path]::GetFileNameWithoutExtension($HostSave) + "_" }
$out = "J:\coop-test\re\runs\player_grab_$tag$Transport"
$hb = if ($Rough) { "pgrab2_host.txt" } else { "pgrab_host.txt" }
$gb = if ($Rough) { "pgrab2_guest.txt" } else { "pgrab_guest.txt" }
$hs = if ($HostSave) { @("test_save=$HostSave") } else { @() }
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$shots = @(); for ($t = 100.0; $t -le 230; $t += 3) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 240 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests $hb)) -GuestBridge @((Join-Path $tests $gb)) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "test_log_heights=1") + $hs + $hostNet + $ExtraIni) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "test_log_heights=1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $guestNet + $ExtraIni) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_player_grab.py") $out)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"player grab suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
