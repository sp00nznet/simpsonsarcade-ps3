Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
}
"@

# PW_CLIENTONLY=1, PW_RENDERFULLCONTENT=2 — OR to capture D3D/GDI content
$PW_RENDERFULLCONTENT = 2

$p = Get-Process simpsons -ErrorAction SilentlyContinue
if (-not $p) { Write-Host "no process"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq 0) { Write-Host "no window handle"; exit 1 }

$rect = New-Object Win+Rect
[Win]::GetWindowRect($h, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$ht = $rect.Bottom - $rect.Top
Write-Host "window: ${w}x${ht} at ($($rect.Left), $($rect.Top))"

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Win]::PrintWindow($h, $hdc, $PW_RENDERFULLCONTENT)
$g.ReleaseHdc($hdc)
Write-Host "PrintWindow ok=$ok"

$out = $args[0]
if (-not $out) { $out = "screen.png" }
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "saved $out"
$bmp.Dispose()
$g.Dispose()
