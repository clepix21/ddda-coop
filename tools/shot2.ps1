# Screenshot of the test copies' own windows side by side (guest | host), half size: each window's own pixels
# (PrintWindow, PW_RENDERFULLCONTENT), never the screen. A screen copy caught the owner's Discord window
# when it covered a game window (2026-10-30 23:40); the owner may be using the PC during the tests.
# Usage: .\shot2.ps1 <out.png>
param([string]$Path)
Add-Type -AssemblyName System.Drawing
if (-not ("Shot2.W" -as [type])) {
    Add-Type -Namespace Shot2 -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
}
function Capture([System.Diagnostics.Process]$p) {
    if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { return $null }
    $r = New-Object Shot2.W+RECT; [Shot2.W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return $null }
    $b = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($b); $dc = $g.GetHdc()
    $ok = [Shot2.W]::PrintWindow($p.MainWindowHandle, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    if (-not $ok) { $b.Dispose(); return $null }
    return $b
}
# the copies: DDDA\ is the host, DDDA2\ the guest (DDDA3\ a second guest, not shown)
$procs = Get-Process DDDA -ErrorAction SilentlyContinue
$hostP = $procs | Where-Object { $_.Path -like "*\DDDA\DDDA.exe" } | Select-Object -First 1
$guestP = $procs | Where-Object { $_.Path -like "*\DDDA2\DDDA.exe" } | Select-Object -First 1
$out = New-Object System.Drawing.Bitmap 1920, 540
$g = [System.Drawing.Graphics]::FromImage($out); $g.Clear([System.Drawing.Color]::Black)
$x = 0
foreach ($p in @($guestP, $hostP)) {
    $b = Capture $p
    if ($b) { $g.DrawImage($b, $x, 0, 960, 540); $b.Dispose() }
    $x += 960
}
$g.Dispose(); $out.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png); $out.Dispose()
