# Every suite in a row (overnight series): stops before a suite when DDDA.exe is running outside the
# tests (the owner playing). Prints each suite's summary line.
$tools = $PSScriptRoot
$suites = @(
    @("suite_shared.ps1"), @("suite_shared.ps1", "-Transport", "mock"), @("suite_revive.ps1"), @("suite_revive_special.ps1"),
    @("suite_skills.ps1"), @("suite_quest.ps1"), @("suite_shared_exp.ps1"), @("suite_area_lead.ps1"), @("suite_area_lead.ps1", "-Look"),
    @("suite_reconnect.ps1"), @("suite_progress.ps1"), @("suite_roam.ps1"), @("suite_enemies.ps1"), @("suite_party.ps1", "-Look"))
foreach ($s in $suites) {
    if (Get-Process DDDA -ErrorAction SilentlyContinue) { "STOP: DDDA.exe is running before $($s -join ' ')"; break }
    $script = Join-Path $tools $s[0]
    $rest = if ($s.Count -gt 1) { $s[1..($s.Count - 1)] } else { @() }
    $out = & $script @rest 2>&1
    $line = $out | Where-Object { $_ -match "suite \(" } | Select-Object -Last 1
    if (-not $line) { $line = "$($s -join ' '): no summary line" }
    "$(Get-Date -Format HH:mm) $line"
}
