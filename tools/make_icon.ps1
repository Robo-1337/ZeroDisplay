# Generates res\app.ico (PNG-compressed entries, 16-256 px) with System.Drawing: the Raster Z, a display's scan
# path (across, the diagonal retrace back, across again) drawn as a Z for ZeroDisplay.
# Run once; the .ico is checked in so normal builds don't need this.
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$images = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $u = $s / 96.0
    # Small icons get relatively thicker strokes so the retrace survives at 16 px.
    if ($s -le 24) { $bar = $s * 0.19; $retrace = [Math]::Max($s * 0.11, 1.6) } else { $bar = 13 * $u; $retrace = 6.5 * $u }
    $top = [single](22 * $u); $bottom = [single](74 * $u); $left = [single](18 * $u); $right = [single](78 * $u)
    if ($s -le 24) { $top = [single]($bar / 2 + $s * 0.08); $bottom = [single]($s - $bar / 2 - $s * 0.08) }
    $blue = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s), ([System.Drawing.Color]::FromArgb(255, 0x8B, 0xE1, 0xFF)), ([System.Drawing.Color]::FromArgb(255, 0x00, 0x63, 0xB8))
    $pen = New-Object System.Drawing.Pen $blue, ([single]$bar)
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawLine($pen, $left, $top, $right, $top)
    $g.DrawLine($pen, $left, $bottom, $right, $bottom)
    $diag = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 0x4C, 0xC2, 0xFF)), ([single]$retrace)
    $diag.StartCap = 'Round'; $diag.EndCap = 'Round'
    $g.DrawLine($diag, [single]($right - 4 * $u), [single]($top + $bar * 0.55), [single]($left + 4 * $u), [single]($bottom - $bar * 0.55))
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $images[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot "..\res\app.ico"), $out.ToArray())
Write-Host "Wrote res\app.ico"
