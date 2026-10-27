# Screenshot of the primary screen at half size. Usage: .\shot.ps1 <out.png>
param([string]$Path)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$r = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $r.Width, $r.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Location, [System.Drawing.Point]::Empty, $r.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($r.Width / 2)), ([int]($r.Height / 2))
$small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
