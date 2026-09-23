# Scan the flatbed with no GUI and write a 24-bit BMP.
#
# The bench instrument for this panel is a Canon PIXMA MG3100 over WIA 2.0, not a camera
# (ticket 20). The device lies face-down in the corner of the bed, so a scan sees the
# panel -- and, since the LEDs went in on 2026-09-03, the two status LEDs on the same face.
#
# WIA.DeviceManager -> Items.Item(1).Transfer() needs no WIA.CommonDialog, which is what
# makes this scriptable at all.
#
# Two things about this device's WIA properties cost an hour between them:
#
#   * The IDs are not what their names suggest in the constants list. On this scanner
#     6149/6150 are the start POSITION, 6151/6152 the EXTENT, 4103 is Data Type, and 6146
#     is Current Intent. Dump $item.Properties rather than trusting a table.
#   * **Extents are in units of 300 dpi, whatever the scan resolution is.** The defaults
#     (2550 x 3507) are the full bed. Setting extent 637 and resolution 75 does not give a
#     637-pixel-wide scan, it gives 159 -- 637/4, because 75 dpi is a quarter of 300. So
#     the arguments here are in INCHES and the conversion happens in one place.
#
# Usage:
#   powershell -NoProfile -File tools/scan_panel.ps1 -Out scan.bmp [-Dpi 75]
#                        [-XInch 0 -YInch 0 -WInch 8.5 -HInch 11.69]
#
# A full bed at 75 dpi takes about 12 s; the panel region at 600 dpi took 13.7 s.

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Dpi = 75,
    [double]$XInch = 0,
    [double]$YInch = 0,
    [double]$WInch = 8.5,
    [double]$HInch = 11.69
)

$ErrorActionPreference = "Stop"
$wiaFormatBMP = "{B96B3CAB-0728-11D3-9D7B-0000F81EF32E}"
$EXTENT_BASE_DPI = 300

$dm = New-Object -ComObject WIA.DeviceManager
$info = $dm.DeviceInfos | Where-Object { $_.Type -eq 1 } | Select-Object -First 1
if ($null -eq $info) { throw "no flatbed scanner found (is the MG3100 powered on?)" }
$item = $info.Connect().Items.Item(1)

# **AN ORDERED DICTIONARY, AND THE ORDER IS GEOMETRY BEFORE RESOLUTION.** This was a plain
# PowerShell hashtable, whose enumeration order is undefined, and the orders are not equivalent:
#
#   * **Writing the resolution rescales the extents' own maxima.** At 300 dpi the bed is
#     2550 x 3507; set 200 dpi and the maxima become 1700 x 2338, so an extent expressed in
#     300-dpi units -- which is what the header above says they are, and what the 75 dpi example
#     proves -- is then out of range and the device throws "parameter is incorrect". Measured
#     2026-09-19 by putting the resolution first and watching only the 200 dpi scans fail.
#   * So the geometry goes in while the device is still at its default 300 dpi, and the resolution
#     goes in last, where it scales the whole rectangle at once.
#
# **AND A NON-ZERO -YInch IS NOT RELIABLE ON THIS SCANNER, WHATEVER THE ORDER.** A region at
# YInch 2.6 asking for 5.1 inches of height returns 3.96: the clamp is on `start_y + extent_y` at
# about 6.64 inches, it is independent of resolution, and four write orders were tried with all
# four clamping. On 2026-09-19 that removed the bottom inch of the reTerminal E1002's panel --
# where the first character of every line of the connect card is -- from a scan that reported
# success. **Scan from YInch 0 and crop with tools/bmp_to_png.ps1's -CropX/-CropY/-CropW/-CropH.**
# The size check at the bottom of this file is what would have caught it on the first scan.
$props = [ordered]@{
    6146 = 0                                       # current intent: none
    4103 = 3                                       # data type: colour
    6154 = 0                                       # brightness
    6155 = 0                                       # contrast
    6151 = [int]($WInch * $EXTENT_BASE_DPI)        # horizontal extent -- GEOMETRY FIRST,
    6152 = [int]($HInch * $EXTENT_BASE_DPI)        # vertical extent      in 300-dpi units,
    6149 = [int]($XInch * $EXTENT_BASE_DPI)        # horizontal start     while the device is
    6150 = [int]($YInch * $EXTENT_BASE_DPI)        # vertical start       still at 300 dpi
    6147 = $Dpi                                    # horizontal resolution -- RESOLUTION LAST
    6148 = $Dpi                                    # vertical resolution
}
foreach ($kv in $props.GetEnumerator()) {
    $p = $item.Properties | Where-Object { $_.PropertyID -eq $kv.Key }
    if ($null -eq $p) { throw "scanner has no property $($kv.Key)" }
    $p.Value = $kv.Value
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$image = $item.Transfer($wiaFormatBMP)
$sw.Stop()

if (Test-Path $Out) { Remove-Item $Out -Force }
$image.SaveFile($Out)

Write-Output ("scanned {0}x{1} at {2} dpi in {3:N1} s -> {4}" -f $image.Width, $image.Height, $Dpi, $sw.Elapsed.TotalSeconds, $Out)

# **Say so when the scanner did not give you the region you asked for.** The order bug above was
# invisible for exactly this reason: the tool reported a size and nothing compared it with the
# request, so a short scan read as a successful one and the missing inch was blamed on the panel.
# A warning rather than a throw, because the image is still usable and the operator may have asked
# for more than the bed has.
$wantW = [int]($WInch * $Dpi)
$wantH = [int]($HInch * $Dpi)
if ([Math]::Abs($image.Width - $wantW) -gt 2 -or [Math]::Abs($image.Height - $wantH) -gt 2) {
    Write-Warning ("REGION SHORT: asked for {0}x{1} px ({2}x{3} inch at {4} dpi), got {5}x{6}" -f
        $wantW, $wantH, $WInch, $HInch, $Dpi, $image.Width, $image.Height)
}
