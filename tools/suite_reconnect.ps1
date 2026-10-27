# Drop and rejoin over the Steam path (test stand-in): the guest's game is killed at 120 s and
# started again 15 s later. The host must drop the silent guest (pawn back to its AI), accept the
# same friend again, and the rejoined guest must drive the pawn along its own camera as before.
# Prints PASS/FAIL lines and a summary; exit 1 on any failure.
param([int]$Loss = 0, [int]$Latency = 0)
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\pair_reconnect"
Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
$mock = @("transport=steam", "steam_mock=1", "mock_loss=$Loss", "mock_latency=$Latency")
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 320 -OutDir $out -RestartGuestAt 120 -RestartDelay 15 `
    -HostBot (Join-Path $tests "bot_net_host.txt") -GuestBot (Join-Path $tests "bot_net_guest_turncam.txt") `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "mock_id=1", "mock_friends=2") + $mock) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "guest_cam_pawn=1", "mock_id=2", "mock_friends=1") + $mock) 2>$null
$run | ForEach-Object { "   $_" }
$hostLog = Get-Content (Join-Path $out "host.log")
$checks = @()
$connects = @($hostLog | Select-String "net: guest connected")
$silent = @($hostLog | Select-String "net: (peer|guest \d+ \(.*\)) silent")
$off = @($hostLog | Select-String "possess: OFF")
$checks += if ($connects.Count -eq 2) { "PASS  host saw the guest connect twice" } else { "FAIL  host saw the guest connect twice (got $($connects.Count))" }
$checks += if ($silent.Count -ge 1 -and $off.Count -ge 1 -and $off[0].LineNumber -gt $silent[0].LineNumber) { "PASS  silent guest dropped, pawn back to its AI" } else { "FAIL  silent guest dropped, pawn back to its AI" }
if ($connects.Count -eq 2 -and $silent.Count -ge 1) {
    $checks += if ($connects[1].LineNumber -gt $silent[0].LineNumber) { "PASS  guest rejoined after the drop" } else { "FAIL  guest rejoined after the drop" }
}
# the rejoined guest's forward push: guest bot 159-164 s after its second start (135 s)
$checks += @(python -I (Join-Path $tools "check_net.py") $out --pawn-window 294 299)
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "a copy exited early") { $checks += "FAIL  both copies still running at the end" } else { $checks += "PASS  both copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"reconnect suite (mock, loss $Loss%, latency $Latency ms): $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
