param([string]$inDir, [string]$out, [int]$fromMs = 0, [int]$toMs = 99999,
      [int]$cols = 4, [int]$cellW = 460, [int]$labelH = 26)
Add-Type -AssemblyName System.Drawing
$rows = New-Object System.Collections.ArrayList
foreach ($line in (Get-Content "$inDir\frames.csv" | Select-Object -Skip 1)) {
    $p = $line.Split(',')
    if ([int]$p[1] -ge [int]$fromMs -and [int]$p[1] -le [int]$toMs) { [void]$rows.Add(@([int]$p[0],[int]$p[1])) }
}
if ($rows.Count -eq 0) { Write-Output "no frames in window"; exit 1 }
$n = [int]$rows.Count
$cols = [int]$cols; $cellW = [int]$cellW; $labelH = [int]$labelH
$gridRows = [int][Math]::Ceiling($n / $cols)
$bmp0 = [System.Drawing.Image]::FromFile("$inDir\frame_{0:d4}.png" -f [int]$rows[0][0])
$cellH = [int]([int]$cellW * [int]$bmp0.Height / [int]$bmp0.Width)
$bmp0.Dispose()
$sheetW = [int]($cols * $cellW); $sheetH = [int]($gridRows * ($cellH + $labelH))
$sheet = New-Object System.Drawing.Bitmap($sheetW, $sheetH)
$gfx = [System.Drawing.Graphics]::FromImage($sheet)
$gfx.Clear([System.Drawing.Color]::Black)
$fnt = New-Object System.Drawing.Font("Consolas", 9)
$i = 0
foreach ($r in $rows) {
    $fr = [int]$r[0]; $t = [int]$r[1]
    $b = [System.Drawing.Image]::FromFile("$inDir\frame_{0:d4}.png" -f $fr)
    $cx = [int](($i % $cols) * $cellW); $cy = [int]([int]($i / $cols) * ($cellH + $labelH))
    $gfx.DrawImage($b, $cx, ($cy + $labelH), $cellW, $cellH)
    $gfx.DrawString("f$fr  ${t}ms", $fnt, [System.Drawing.Brushes]::White, $cx + 2, ($cy + 3))
    $b.Dispose(); $i++
}
$gfx.Dispose()
$sheet.Save($out)
Write-Output "sheet: $n frames ($fromMs-$toMs ms) -> $out"
