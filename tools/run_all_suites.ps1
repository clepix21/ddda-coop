# Every suite in a row (overnight series): stops before a suite when DDDA.exe is running outside the
# tests (the owner playing). Prints each suite's last line (its summary).
$tools = $PSScriptRoot
$suites = "suite_shared.ps1", "suite_shared.ps1 -Transport mock", "suite_revive.ps1", "suite_revive_special.ps1",
    "suite_skills.ps1", "suite_quest.ps1", "suite_shared_exp.ps1", "suite_area_lead.ps1", "suite_area_lead.ps1 -Look",
    "suite_reconnect.ps1", "suite_progress.ps1", "suite_roam.ps1", "suite_enemies.ps1", "suite_party.ps1 -Look", "suite_area_exit.ps1", "suite_player_grab.ps1", "suite_player_grab.ps1 -Rough -HostSave J:\coop-test\saves\merchant.sav", "suite_cutscene.ps1 -Shared", "suite_revive.ps1 -Tap", "suite_menu_world.ps1"
foreach ($s in $suites) {
    if (Get-Process DDDA -ErrorAction SilentlyContinue) { "STOP: DDDA.exe is running before $s"; break }
    $parts = $s -split ' '
    $script = Join-Path $tools $parts[0]
    $named = @{} # "-Transport mock" -> Transport = mock, "-Look" -> Look = true (a string array binds by position)
    for ($i = 1; $i -lt $parts.Count; $i++) {
        $k = $parts[$i].TrimStart('-')
        if ($i + 1 -lt $parts.Count -and -not $parts[$i + 1].StartsWith('-')) { $named[$k] = $parts[$i + 1]; $i++ } else { $named[$k] = $true }
    }
    $out = & $script @named 2>&1 | ForEach-Object { "$_" }
    $line = $out | Where-Object { $_ -match "checks passed" } | Select-Object -Last 1
    if (-not $line) { $line = "${s}: no summary; last lines: " + (($out | Select-Object -Last 3) -join ' | ') }
    "$(Get-Date -Format HH:mm) $line"
}
