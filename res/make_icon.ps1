# Generates res\app.ico for Kontur.
Add-Type -AssemblyName System.Drawing
$sizes = 16, 24, 32, 48, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $u = $s / 16.0
    $g.ScaleTransform($u, $u)
    $base = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(31, 30, 45))
    $accent = [System.Drawing.Color]::FromArgb(184, 167, 255)
    $trace = [System.Drawing.Pen]::new($accent, 1.6)
    $node = [System.Drawing.SolidBrush]::new($accent)
    $g.FillEllipse($base, 0, 0, 16, 16)
    $g.DrawArc($trace, 3.25, 3.25, 9.5, 9.5, 42, 276)
    $g.FillEllipse($node, 10.2, 6.8, 2.4, 2.4)
    $node.Dispose()
    $trace.Dispose()
    $base.Dispose()
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'app.ico'), $out.ToArray())
