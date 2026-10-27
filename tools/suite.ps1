# Regression suite: runs each scenario in the test copy (saves blocked, bot only) and checks the
# probe log with analyze_log.py. Prints one PASS/FAIL line per check and a summary.
# Usage: .\suite.ps1 [-Only name]
param([string]$Only = "")
$tools = $PSScriptRoot
$tests = Join-Path $PSScriptRoot "..\tests"
$scenarios = @(
    @{ Name = "stick_and_buttons"; Bot = "bot_possess_buttons.txt"; Seconds = 162; Keys = @();
       Ini = @("p2_pad=1", "idle_pad=3", "auto_possess=133000");
       Windows = @("p2_back 135 141", "idle 141.5 146", "p2_right 146 150");
       Expect = @("p2_back pawn > 100", "p2_back arisen < 5", "idle pawn < 5", "p2_right pawn > 100",
                  "p2_right arisen < 5", "press pawn 1 3", "press pawn 2 1", "press pawn 2000 1", "nopress arisen") },
    @{ Name = "keyboard_hidden_from_pawn"; Bot = "bot_kbm.txt"; Seconds = 145; Keys = @("139:1F:3");
       Ini = @("p2_pad=1", "idle_pad=3", "auto_possess=133000");
       Windows = @("p2_right 135 137", "keyboard 138.5 142.5");
       Expect = @("p2_right pawn > 100", "p2_right arisen < 5", "keyboard arisen > 300", "keyboard pawn < 5") },
    @{ Name = "toggle_on_off"; Bot = "bot_toggle.txt"; Seconds = 150; Keys = @();
       Ini = @("p2_pad=1", "idle_pad=3", "auto_possess=0");
       Windows = @("on_right 133 136", "off_right 142 145");
       Expect = @("on_right pawn > 100", "on_right arisen < 5", "off_right arisen < 5", "off_right pawn < 5") },
    @{ Name = "combat_underground"; Bot = "bot_load_game_p2.txt"; Seconds = 300; Keys = @();
       Ini = @("p2_pad=1", "idle_pad=3", "bridge=1");
       Bridge = @("route_guild_to_underground.txt", "combat_actions.txt");
       Windows = @();
       Expect = @("press pawn 1 6", "press pawn 2 2", "press pawn 28001 2", "pressmax arisen 3 0") }
)
$total = 0; $failed = 0; $results = @()
foreach ($s in $scenarios) {
    if ($Only -and $s.Name -ne $Only) { continue }
    $out = "J:\coop-test\re\runs\$($s.Name)"
    "== $($s.Name)"
    $bridge = @(); if ($s.Bridge) { $bridge = $s.Bridge | ForEach-Object { Join-Path $tests $_ } }
    $run = & (Join-Path $tools "run_test.ps1") -Seconds $s.Seconds -Bot (Join-Path $tests $s.Bot) -Keys $s.Keys -Ini $s.Ini -OutDir $out -BridgeScripts $bridge
    $run | ForEach-Object { "   $_" }
    $args = @((Join-Path $out "coop.log"))
    foreach ($w in $s.Windows) { $args += "--window"; $args += $w.Split(' ') }
    foreach ($e in $s.Expect) { $args += "--expect"; $args += $e }
    $checks = python -I (Join-Path $tools "analyze_log.py") @args | Where-Object { $_ -match "^(PASS|FAIL)" }
    if (-not ($run -match "saves unchanged: True")) { $checks += "FAIL  owner's save folder unchanged" } else { $checks += "PASS  owner's save folder unchanged" }
    if (-not ($run -match "guard active: True")) { $checks += "FAIL  save guard active" } else { $checks += "PASS  save guard active" }
    if ($run -match "game exited early: True") { $checks += "FAIL  game still running at the end" } else { $checks += "PASS  game still running at the end" }
    foreach ($c in $checks) { "   $c"; $total++; if ($c -like "FAIL*") { $failed++ } }
    # keep a failing run (the next run of the scenario overwrites its folder): intermittent failures
    if ($checks -like "FAIL*") { $keep = "$out`_fail_$(Get-Date -Format 'yyyyMMdd_HHmmss')"; Copy-Item $out $keep -Recurse; "   kept as $keep" }
}
""
"suite: $($total - $failed)/$total checks passed, $failed failed ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
if ($failed) { exit 1 }
