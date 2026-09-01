# Drive the game with synthetic key presses and screenshot each step.
#   powershell -File tools\drive.ps1 <outprefix> <key> [<key> ...]
# Keys: ENTER Z X A S UP DOWN LEFT RIGHT TAB  (see the cellPad keyboard map)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
  public struct Rect { public int Left; public int Top; public int Right; public int Bottom; }
}
"@
$VK = @{ 'ENTER'=0x0D; 'Z'=0x5A; 'X'=0x58; 'A'=0x41; 'S'=0x53; 'TAB'=0x09;
         'UP'=0x26; 'DOWN'=0x28; 'LEFT'=0x25; 'RIGHT'=0x27 }

$prefix = $args[0]
$keys   = $args[1..($args.Count-1)]

$p = Get-Process simpsons -ErrorAction SilentlyContinue
if (-not $p) { Write-Host "not running"; exit 1 }
$h = $p.MainWindowHandle
[W]::ShowWindow($h, 9) | Out-Null
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700

function Shoot($path) {
  $r = New-Object W+Rect
  [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap (($r.Right-$r.Left), ($r.Bottom-$r.Top))
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [W]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Host ("{0}  {1} bytes" -f $path, (Get-Item $path).Length)
}

$i = 0
foreach ($k in $keys) {
  $i++
  # Press and hold long enough for the game's ~4ms pad poll to see several packets.
  $code = $VK[$k.ToUpper()]
  # Re-assert focus every key: the pad's keyboard fallback only reads keys while
  # a window of the game's process is in the foreground.
  [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 150
  [W]::keybd_event($code,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 250
  [W]::keybd_event($code,0,2,[UIntPtr]::Zero); Start-Sleep -Milliseconds 1500
  Shoot ("{0}_{1:d2}_{2}.png" -f $prefix, $i, $k)
}
