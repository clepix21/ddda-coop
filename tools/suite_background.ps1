# Shared world, two copies: the guest's game window goes to the background (WM_ACTIVATEAPP(FALSE) at 150 s,
# as an alt-tab or a click on another screen does) for 40 s. Issue #3 (2026-11-11): the game stopped there,
# no frame at all, and the others saw it frozen. In co-op its frames must go on, its pads idle meanwhile.
# -Paused: pause_in_background=1 (the game's own pause, to see the test catch it). Prints PASS/FAIL lines.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Paused)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\background_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$bg = @("keep_active=0", "test_background_at=150", "test_background_for=40") + $(if ($Paused) { @("pause_in_background=1") } else { @() })
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 230 -OutDir $out -Shots 170 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "shared_guest_walk.txt") `
    -HostBridge @((Join-Path $tests "talk_wait.txt")) `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "shared_world=1", "test_save=J:\coop-test\saves\guest_arisen.sav") + $guestNet + $bg) 2>$null
$run | ForEach-Object { "   $_" }
$g = Get-Content (Join-Path $out "guest.log"); $h = Get-Content (Join-Path $out "host.log")
$checks = @()
function Check([bool]$ok, [string]$what) { if ($ok) { "PASS  $what" } else { "FAIL  $what" } }
function T([string]$line) { [TimeSpan]::Parse($line.Substring(0, 12)) }
$sent = @($g | Select-String "window: tests: WM_ACTIVATEAPP\(FALSE\)")
$back = @($g | Select-String "window: tests: WM_ACTIVATEAPP\(TRUE\)")
$checks += Check ($sent.Count -eq 1 -and $back.Count -eq 1) "the guest's window sent to the background and back"
$checks += Check (@($g | Select-String "window: in the background: the game goes on").Count -ge 1 -and -not $Paused -or $Paused) "the guest's game says it goes on in the background"
$minFrames = -1
if ($sent.Count -and $back.Count) {
    $t0 = T $sent[0].Line; $t1 = T $back[0].Line
    $perf = @($g | Select-String "perf: 10 s, (\d+) frames" | Where-Object { $t = T $_.Line; $t -gt $t0.Add([TimeSpan]::FromSeconds(10)) -and $t -le $t1 })
    if ($perf.Count) { $minFrames = ($perf | ForEach-Object { [int]$_.Matches[0].Groups[1].Value } | Measure-Object -Minimum).Minimum }
    $checks += Check ($minFrames -ge 300) "the guest's frames go on in the background (fewest in 10 s: $minFrames, over $($perf.Count) counts)"
    $hostSees = @($h | Select-String "party \d shows another player, error [\d.]+ u before correction, HP [\d.]+ \(state (\d+) ms old" | Where-Object { $t = T $_.Line; $t -gt $t0.Add([TimeSpan]::FromSeconds(5)) -and $t -le $t1 })
    $fresh = @($hostSees | Where-Object { [int]$_.Matches[0].Groups[1].Value -lt 1000 })
    $checks += Check ($hostSees.Count -ge 10 -and $fresh.Count -ge $hostSees.Count - 2) "the host gets the guest's states meanwhile ($($fresh.Count) fresh of $($hostSees.Count))"
}
$after = @($g | Select-String "window: active again")
$checks += Check ($after.Count -ge 1 -or $Paused) "the guest's game active again after"
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"background suite ($Transport$(if ($Paused) { ', paused' })): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
