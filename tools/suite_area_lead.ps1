# Shared world, three copies: the host stays in the Pawn Guild; both guests walk to the underground (stage
# 600, small creatures) where the host is not. Guest 1 (the lowest slot there) must run the enemies of that
# group and guest 2 must follow them (pairs with guest 1's enemies, no removals near it). Prints PASS/FAIL lines.
# -Latency: mock transport only, ms added each way
# -Look: guest 1 plays another character (guest_arisen.sav, Arisen "Invite")
param([ValidateSet("udp", "mock")][string]$Transport = "udp", [switch]$Look, [int]$Latency = 0, [int]$Jitter = 0, [string[]]$ExtraIni = @())
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\area_lead_$Transport"
if ($Transport -eq "udp") {
    $hostNet = @("transport=udp"); $g1Net = @("transport=udp", "peer=127.0.0.1"); $g2Net = $g1Net
} else {
    Remove-Item "J:\coop-test\mock_steam\*" -ErrorAction SilentlyContinue
    $mock = @("transport=steam", "steam_mock=1", "mock_latency=$Latency", "mock_jitter=$Jitter")
    $hostNet = $mock + @("mock_id=1", "mock_friends=2,3")
    $g1Net = $mock + @("mock_id=2", "mock_friends=1"); $g2Net = $mock + @("mock_id=3", "mock_friends=1")
}
$common = @("p2_pad=1", "idle_pad=3", "port=27101", "shared_world=1")
$guest = $common + @("role=guest", "bridge=1", "test_save=J:\coop-test\saves\guest_arisen.sav")
$route = Join-Path $tests "route_guild_to_underground.txt"
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 380 -OutDir $out -Shots 300, 340 -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "shared_guest_load.txt") `
    -Guest2Bot (Join-Path $tests "shared_guest_load.txt") -GuestBridge @($route) -Guest2Bridge @((Join-Path $tests "wait_25s.txt"), $route) `
    -HostIni ($common + @("role=host", "bridge=1") + $hostNet) -GuestIni ($guest + $g1Net + $(if ($Look) { @("test_save=J:\coop-test\saves\guest_arisen.sav") } else { @() }) + $ExtraIni) -Guest2Ini ($guest + $g2Net + $ExtraIni) 2>$null
$run | ForEach-Object { "   $_" }
$g1 = Get-Content (Join-Path $out "guest.log"); $g2 = Get-Content (Join-Path $out "guest2.log"); $h = Get-Content (Join-Path $out "host.log")
$checks = @()
function Check([bool]$ok, [string]$what) { if ($ok) { "PASS  $what" } else { "FAIL  $what" } }
$checks += Check (@($h | Select-String "net: guest connected").Count -ge 2) "both guests connected"
$g1in = @($g1 | Select-String "stage: load 600"); $g2in = @($g2 | Select-String "stage: load 600")
$checks += Check ($g1in.Count -ge 1 -and $g2in.Count -ge 1) "both guests reached the underground (600)"
$lead1 = @($g1 | Select-String "enemies around this player: this game runs them")
$checks += Check ($lead1.Count -ge 1) "guest 1 runs the enemies of its group"
$lead2 = @($g2 | Select-String "enemies: player 1 leads this group \(stage 600\)")
$checks += Check ($lead2.Count -ge 1) "guest 2 follows guest 1's enemies in 600"
$pairs = if ($lead2) { @($g2 | Select-Object -Skip $lead2[0].LineNumber | Select-String "paired with host #") } else { @() }
$checks += Check ($pairs.Count -ge 1) "guest 2 paired its enemies with guest 1's ($($pairs.Count) pairs)"
$removed = if ($lead2) { @($g2 | Select-Object -Skip $lead2[0].LineNumber | Select-String "not on the host for 10 s, removed") } else { @() }
"   info: guest 2 removed $($removed.Count) enemies of its own after following guest 1"
$checks += Check ([bool]($run -match "saves and owner config unchanged: True")) "owner's saves and config unchanged"
$checks += Check (-not ($run -match "exited early True")) "all copies still running at the end"
$failed = ($checks | Where-Object { $_ -like "FAIL*" }).Count
$checks | ForEach-Object { "   $_" }
"area lead suite ($Transport): $($checks.Count - $failed)/$($checks.Count) checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
