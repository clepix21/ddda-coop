# Full-resolution crop of the screen, scaled by -Zoom. Usage: .\shot_crop.ps1 <out.png> x y w h [-Zoom 2]
param([string]$Path, [int]$X, [int]$Y, [int]$W, [int]$H, [int]$Zoom = 2)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
$big = New-Object System.Drawing.Bitmap $bmp, ($W * $Zoom), ($H * $Zoom)
$big.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $big.Dispose()
