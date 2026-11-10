# shot.ps1 - capture a window of a given process with PrintWindow(PW_RENDERFULLCONTENT)
# into a PNG. Works while the window is covered by other windows (DWM content).
# Usage: tools\shot.ps1 -ProcessId 1234 -Out shots\x.png
# Only ever point this at test_host.exe - never at the game.
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Out, [switch]$ClientOnly)
Add-Type -AssemblyName System.Drawing
if (-not ('ShotNative' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShotNative {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
}
[ShotNative]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Id $ProcessId -ErrorAction Stop
if ($p.ProcessName -like 'CivilizationV*') { throw 'refusing to capture the game' }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "process $ProcessId has no main window" }
$r = New-Object ShotNative+RECT
[ShotNative]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [ShotNative]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
if ($ClientOnly) {
  $c = New-Object ShotNative+RECT; [ShotNative]::GetClientRect($h, [ref]$c) | Out-Null
  $pt = New-Object ShotNative+POINT; [ShotNative]::ClientToScreen($h, [ref]$pt) | Out-Null
  $crop = New-Object System.Drawing.Rectangle ($pt.X - $r.L), ($pt.Y - $r.T), $c.R, $c.B
  $bmp2 = $bmp.Clone($crop, $bmp.PixelFormat); $bmp.Dispose(); $bmp = $bmp2
}
$dir = Split-Path -Parent $Out; if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"PrintWindow=$ok size=${w}x${hh} -> $Out"
