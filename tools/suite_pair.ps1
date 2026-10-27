# Two-copy network scenario (host on the middle screen, guest on the left): the host's bot moves
# the Arisen, the guest turns its own camera then pushes its stick forward. Checks with
# check_net.py (connection, convergence, stick relative to the guest's camera) plus the run's own
# save/config check. Prints PASS/FAIL lines and a summary; exit 1 on any failure.
# -Transport udp: plain UDP on 127.0.0.1. -Transport mock: the Steam path (steam_link.cpp) over the
# test-only Steam stand-in (steam_mock.cpp), fake accounts 1 (host) and 2 (guest) that are friends.
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [int]$Loss = 0, [int]$Latency = 0)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\pair_suite_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue   # stale presence from an earlier run
    $mock = @("steam_mock=1", "mock_loss=$Loss", "mock_latency=$Latency")
    $hostNet = @("transport=steam", "mock_id=1", "mock_friends=2") + $mock
    $guestNet = @("transport=steam", "mock_id=2", "mock_friends=1") + $mock
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 175 -OutDir $out `
    -HostBot (Join-Path $tests "bot_net_host.txt") -GuestBot (Join-Path $tests "bot_net_guest_turncam.txt") `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "guest_cam_pawn=1") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$checks = @(python -I (Join-Path $tools "check_net.py") $out --pawn-window 164 169)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if (($run -match "guard active True").Count -eq 2) { $checks += "PASS  save guard active on both copies" } else { $checks += "FAIL  save guard active on both copies" }
if ($run -match "exited early True") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"pair suite ($Transport, loss $Loss%, latency $Latency ms): $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
