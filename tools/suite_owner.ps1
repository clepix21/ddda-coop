# Two real Steam accounts: the guest's game loads the HOST's save, which belongs to another account.
# The two test copies share one account, so test_owner= makes the save the guest receives belong to
# another SteamID. With the owner check patch the guest must reach the host's area (without it, its
# game went back to the title: re/runs/owner_nopatch, 2026-10-27 14:33). Prints PASS/FAIL lines.
param([ValidateSet("udp", "mock")][string]$Transport = "udp")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\owner_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $guestNet = @("transport=udp", "peer=127.0.0.1")
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $hostNet = @("transport=steam", "steam_mock=1", "mock_id=1", "mock_friends=2")
    $guestNet = @("transport=steam", "steam_mock=1", "mock_id=2", "mock_friends=1")
}
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 170 -OutDir $out -Shots 160 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostIni (@("p2_pad=1", "idle_pad=3", "role=host", "port=27101") + $hostNet) `
    -GuestIni (@("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "test_owner=76561198000000001") + $guestNet) 2>$null
$run | ForEach-Object { "   $_" }
$g = Get-Content (Join-Path $out "guest.log")
$checks = @()
$checks += if ($g -match "save owner check patched") { "PASS  owner check patched on the guest" } else { "FAIL  owner check patched on the guest" }
$checks += if ($g -match "test_owner: the host's save owner") { "PASS  the host's save made another account's" } else { "FAIL  the host's save made another account's" }
$checks += if ($g -match "net: stage: guest now in 220") { "PASS  guest loaded the other account's save and reached the host's area" } else { "FAIL  guest loaded the other account's save and reached the host's area" }
$checks += if ($run -match "saves and owner config unchanged: True") { "PASS  owner's saves and config unchanged" } else { "FAIL  owner's saves and config unchanged" }
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"owner suite ($Transport): $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
