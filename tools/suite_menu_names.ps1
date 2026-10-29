# Names in the menus (shared world, screenshots for review): the host (guild.sav, "Clement") and a
# guest (guest_arisen.sav, "Invite") open the game menu once in game; every copy logs the names in both
# player data copies of the save it loads.
# -GuestSave: the guest's own save (default guest_arisen.sav, "Invite"; guild_sorcerer.sav: another face and sex)
param([string]$GuestSave = "J:\coop-test\saves\guest_arisen.sav")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$out = "J:\coop-test\re\runs\menu_names"
$shots = @(); for ($t = 105.0; $t -le 150; $t += 3) { $shots += $t }
$run = & (Join-Path $tools "run_pair.ps1") -Seconds 170 -OutDir $out -Shots $shots -GuestSavesFromRole `
    -HostBot (Join-Path $tests "bot_load_game_p2.txt") -GuestBot (Join-Path $tests "bot_load_game_p2.txt") `
    -HostBridge @((Join-Path $tests "menu_host.txt")) -GuestBridge @((Join-Path $tests "menu_guest.txt")) `
    -HostIni @("p2_pad=1", "idle_pad=3", "role=host", "port=27101", "bridge=1", "shared_world=1", "transport=udp") `
    -GuestIni @("p2_pad=1", "idle_pad=3", "role=guest", "port=27101", "bridge=1", "shared_world=1", "transport=udp", "peer=127.0.0.1", "test_save=$GuestSave") 2>$null
$run | ForEach-Object { "   $_" }
