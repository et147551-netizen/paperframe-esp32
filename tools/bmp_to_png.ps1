# Convert a BMP (what tools/scan_panel.ps1 writes) to PNG, so the Read tool can display it.
#
# There is no PIL on this host and the Read tool does not display BMP, so a scan cannot be
# looked at without this step. System.Drawing is present -- it is what generated the rotation
# fixtures in .scratch/digital-frame/fixtures/.
#
# Usage:
#   powershell -NoProfile -File tools/bmp_to_png.ps1 -In scan.bmp -Out scan.png [-MaxEdge 1400]
#
# -MaxEdge downsamples the long edge so a 300 dpi panel scan is a reasonable size to display;
# 0 keeps the original pixels.

# Crop is in SOURCE PIXELS and is applied before the scale to MaxEdge. It exists because
# tools/scan_panel.ps1 must be given YInch 0 on this scanner -- a non-zero vertical start position
# silently shortens the region, see the note in that file -- so a scan of the reTerminal E1002,
# which lies further down the bed, has to take the whole strip and cut it here. Two panels on one
# bed is also now the normal case (ticket 66).
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$MaxEdge = 1400,
    [int]$CropX = 0,
    [int]$CropY = 0,
    [int]$CropW = 0,
    [int]$CropH = 0
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile((Resolve-Path $In).Path)
try {
    if ($CropW -gt 0 -and $CropH -gt 0) {
        $rect = New-Object System.Drawing.Rectangle $CropX, $CropY, $CropW, $CropH
        $cropped = New-Object System.Drawing.Bitmap $CropW, $CropH
        $gc = [System.Drawing.Graphics]::FromImage($cropped)
        $gc.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $CropW, $CropH), $rect,
                      [System.Drawing.GraphicsUnit]::Pixel)
        $gc.Dispose()
        $src.Dispose()
        $src = $cropped
    }
    $w = $src.Width
    $h = $src.Height
    $scale = 1.0
    if ($MaxEdge -gt 0) {
        $long = [Math]::Max($w, $h)
        if ($long -gt $MaxEdge) { $scale = $MaxEdge / $long }
    }
    $nw = [int][Math]::Round($w * $scale)
    $nh = [int][Math]::Round($h * $scale)

    $dst = New-Object System.Drawing.Bitmap $nw, $nh
    try {
        $g = [System.Drawing.Graphics]::FromImage($dst)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($src, 0, 0, $nw, $nh)
        $g.Dispose()
        if (Test-Path $Out) { Remove-Item $Out -Force }
        $dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $dst.Dispose() }
} finally { $src.Dispose() }

Write-Output ("{0} {1}x{2} -> {3} {4}x{5}" -f $In, $w, $h, $Out, $nw, $nh)
