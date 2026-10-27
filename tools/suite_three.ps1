# Three players (host + 2 guests, three copies: DDDA, DDDA2, DDDA3; the guests share the left
# screen). Guest 1 must get hired pawn 1 and guest 2 hired pawn 2 (pawn_order 1,2,0). Guest 1 turns
# its camera and pushes forward at 160 s, guest 2 pushes forward at 175 s (its own clock). Checks for
# each guest: connected with the right pawn, its pawn moves along ITS camera, the Arisen and its pawn
# follow the host on its screen; and each guest sees the OTHER guest's pawn where the host has it.
# Prints PASS/FAIL lines and a summary; exit 1 on any failure.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\three_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $g1Net = @("transport=udp", "peer=127.0.0.1"); $g2Net = $g1Net
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $mock = @("transport=steam", "steam_mock=1")
    $hostNet = $mock + @("mock_id=1", "mock_friends=2,3")
    $g1Net = $mock + @("mock_id=2", "mock_friends=1"); $g2Net = $mock + @("mock_id=3", "mock_friends=1")
}
$common = @("p2_pad=1", "idle_pad=3", "port=27101")
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 215 -OutDir $out -Shots 170, 190, 205 `
    -HostBot (Join-Path $tests "bot_net_host.txt") -GuestBot (Join-Path $tests "bot_net_guest_turncam.txt") `
    -Guest2Bot (Join-Path $tests "bot_net_guest2_fwd.txt") `
    -HostIni ($common + @("role=host") + $hostNet) `
    -GuestIni ($common + @("role=guest", "guest_cam_pawn=1") + $g1Net) `
    -Guest2Ini ($common + @("role=guest", "guest_cam_pawn=1") + $g2Net) 2>$null
$run | ForEach-Object { "   $_" }
$hostLog = Get-Content (Join-Path $out "host.log")
$checks = @()
$c1 = $hostLog | Select-String "net: guest connected .*\(guest 1, pad slot 1, pawn 1\)"
$c2 = $hostLog | Select-String "net: guest connected .*\(guest 2, pad slot 2, pawn 2\)"
$checks += if ($c1) { "PASS  guest 1 connected and got hired pawn 1" } else { "FAIL  guest 1 connected and got hired pawn 1" }
$checks += if ($c2) { "PASS  guest 2 connected and got hired pawn 2" } else { "FAIL  guest 2 connected and got hired pawn 2" }
# each guest's own pawn: it follows the host on that guest's screen and moves along that guest's camera
$checks += @(python -I (Join-Path $tools "check_net.py") $out --pawn-window 164 169 --cmc 1 --guest-log guest.log --guest-num 1 | Where-Object { $_ -match "^(PASS|FAIL)" -and $_ -notmatch "connected|possession" } | ForEach-Object { $_ -replace "^(PASS|FAIL)  ", '$1  guest 1: ' })
$checks += @(python -I (Join-Path $tools "check_net.py") $out --pawn-window 184 190 --cmc 2 --guest-log guest2.log --guest-num 2 | Where-Object { $_ -match "^(PASS|FAIL)" -and $_ -notmatch "connected|possession" } | ForEach-Object { $_ -replace "^(PASS|FAIL)  ", '$1  guest 2: ' })
# each guest sees the other guest's pawn where the host has it
$x1 = @(python -I (Join-Path $tools "compare_pair.py") (Join-Path $out "host.log") (Join-Path $out "guest.log") --cmc 2) | Where-Object { $_ -match "pawn:" }
$x2 = @(python -I (Join-Path $tools "compare_pair.py") (Join-Path $out "host.log") (Join-Path $out "guest2.log") --cmc 1) | Where-Object { $_ -match "pawn:" }
$checks += @($x1 | ForEach-Object { $_ -replace "^(PASS|FAIL)  pawn:", '$1  guest 1 sees guest 2''s pawn:' })
$checks += @($x2 | ForEach-Object { $_ -replace "^(PASS|FAIL)  pawn:", '$1  guest 2 sees guest 1''s pawn:' })
if ($run -match "saves and owner config unchanged: True") { $checks += "PASS  owner's saves and config unchanged" } else { $checks += "FAIL  owner's saves and config unchanged" }
if ($run -match "exited early True") { $checks += "FAIL  all copies still running at the end" } else { $checks += "PASS  all copies still running at the end" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
$total = ($checks | Where-Object { $_ -like "PASS*" -or $_ -like "FAIL*" }).Count
"three-player suite ($Transport): $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
