# Screenshot of the left and middle screens side by side (guest | host), half size.
# Usage: .\shot2.ps1 <out.png>
param([string]$Path)
Add-Type -AssemblyName System.Drawing
$w = 3840; $h = 1080
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen(-1920, 0, 0, 0, (New-Object System.Drawing.Size $w, $h))
$small = New-Object System.Drawing.Bitmap $bmp, 1920, 540
$small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
