# 生成 resources\stargazer.ico：Win11 深色圆角底 + 强调色星星。
# 只依赖 System.Drawing（.NET 自带），改设计就改这里的常量再跑一次：
#   powershell -NoProfile -ExecutionPolicy Bypass -File resources\make-icon.ps1
#
# 小尺寸（<=20px）用四角星：五角星在 16px 下星角会糊成一团，四角星一眼就能认出来。
param([string]$Out = (Join-Path $PSScriptRoot 'stargazer.ico'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bg      = [System.Drawing.Color]::FromArgb(255, 0x20, 0x20, 0x20)  # Theme::bg
$accent  = [System.Drawing.Color]::FromArgb(255, 0x60, 0xCD, 0xFF)  # Theme::accent
$sizes   = @(16, 24, 32, 48, 64, 128, 256)
$pngAt   = 128   # 这个尺寸及以上用 PNG 压缩（Vista+ 支持；小尺寸用经典 BMP 更保险）

# 星形顶点：外半径 outer、内半径 inner，顶点朝上；n=5 五角星，n=4 四角星
function New-Star([float]$cx, [float]$cy, [float]$outer, [float]$inner, [int]$n, [float]$rot) {  $pts = New-Object 'System.Drawing.PointF[]' ($n * 2)
  for ($i = 0; $i -lt $n * 2; $i++) {
    $r = if ($i % 2 -eq 0) { $outer } else { $inner }
    $a = $rot + [Math]::PI * $i / $n
    $pts[$i] = New-Object System.Drawing.PointF([float]($cx + $r * [Math]::Sin($a)),
                                                [float]($cy - $r * [Math]::Cos($a)))
  }
  return ,$pts
}

function New-IconBitmap([int]$s) {
  $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  # 圆角方底（内缩一点点，免得边缘被系统裁掉）
  $inset = [float]([Math]::Max(1.0, $s * 0.045))
  $r = [float]($s * 0.24)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $w = $s - 2 * $inset
  $path.AddArc($inset, $inset, $r, $r, 180, 90)
  $path.AddArc($inset + $w - $r, $inset, $r, $r, 270, 90)
  $path.AddArc($inset + $w - $r, $inset + $w - $r, $r, $r, 0, 90)
  $path.AddArc($inset, $inset + $w - $r, $r, $r, 90, 90)
  $path.CloseFigure()
  $g.FillPath((New-Object System.Drawing.SolidBrush($bg)), $path)

  # 星：中心略上移（五角星的重心偏下，不挪看着是歪的）
  $cx = [float]($s / 2.0)
  $cy = [float]($s / 2.0 - $s * 0.015)
  if ($s -le 20) {
    $pts = New-Star $cx $cy ([float]($s * 0.36)) ([float]($s * 0.11)) 4 0.0
  } else {
    $pts = New-Star $cx $cy ([float]($s * 0.33)) ([float]($s * 0.33 * 0.44)) 5 0.0
  }
  $g.FillPolygon((New-Object System.Drawing.SolidBrush($accent)), $pts)

  $g.Dispose()
  $path.Dispose()
  return $bmp
}

# 32bpp 位图的 BGRA 像素（自下而上），以及 1bpp 的 AND 掩码（全 0：靠 alpha 通道）
function Get-BmpEntry([System.Drawing.Bitmap]$bmp, [int]$s) {
  $ms = New-Object System.IO.MemoryStream
  $bw = New-Object System.IO.BinaryWriter($ms)
  # BITMAPINFOHEADER
  $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
  $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
  $bw.Write([uint32]($s * $s * 4)); $bw.Write([int32]0); $bw.Write([int32]0)
  $bw.Write([uint32]0); $bw.Write([uint32]0)
  for ($y = $s - 1; $y -ge 0; $y--) {
    for ($x = 0; $x -lt $s; $x++) {
      $c = $bmp.GetPixel($x, $y)
      $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
    }
  }
  $maskRow = [int]([Math]::Floor(($s + 31) / 32) * 4)
  $bw.Write((New-Object byte[] ($maskRow * $s)))
  $bw.Flush()
  return $ms.ToArray()
}

function Get-PngEntry([System.Drawing.Bitmap]$bmp) {
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  return $ms.ToArray()
}

$entries = @()
foreach ($s in $sizes) {
  $bmp = New-IconBitmap $s
  $png = $s -ge $pngAt
  $entries += [pscustomobject]@{ size = $s; png = $png; data = $(if ($png) { Get-PngEntry $bmp } else { Get-BmpEntry $bmp $s }) }
  $bmp.Dispose()
}

# 注意：PowerShell 变量名不分大小写，$out 会和参数 $Out 撞成同一个 —— 所以这里叫 $stream
$stream = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter -ArgumentList $stream
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
  $w.Write([byte]$(if ($e.size -ge 256) { 0 } else { $e.size }))
  $w.Write([byte]$(if ($e.size -ge 256) { 0 } else { $e.size }))
  $w.Write([byte]0); $w.Write([byte]0)
  $w.Write([uint16]1); $w.Write([uint16]32)
  $w.Write([uint32]$e.data.Length); $w.Write([uint32]$offset)
  $offset += $e.data.Length
}
foreach ($e in $entries) { $w.Write([byte[]]$e.data) }   # 必须显式转型：否则 PowerShell 会挑 Write(byte) 只写一个字节
$w.Flush()
[IO.File]::WriteAllBytes($Out, $stream.ToArray())

# 回读校验：能解析、尺寸对、星是强调色、角落是深色底
$ico = New-Object System.Drawing.Icon($Out, 32, 32)
$bmp = $ico.ToBitmap()
$mid = $bmp.GetPixel([int]($bmp.Width / 2), [int]($bmp.Height / 2))
$corner = $bmp.GetPixel(1, 1)
"$Out  $([int]((Get-Item $Out).Length / 1KB)) KB  共 $($entries.Count) 个尺寸"
"32px 中心像素 = R$($mid.R) G$($mid.G) B$($mid.B)（期望接近强调色 96/205/255）"
"32px 角落像素 = R$($corner.R) G$($corner.G) B$($corner.B) A$($corner.A)（期望深色底或透明）"
if ($mid.B -lt 200 -or $mid.R -gt 140) { throw "图标中心不是强调色 —— 生成有问题" }
