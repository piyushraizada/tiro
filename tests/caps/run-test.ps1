param([string]$mode, [string]$outDir)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Language CSharp @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinApi3 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public static IntPtr Find(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h,l) => { if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(256); GetWindowTextW(h, sb, 256);
            if (sb.ToString() == title) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
"@
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$repo = "D:\source\repos\Tiro"
$null = Start-Process -FilePath "$repo\bin\Release\Tiro.exe" -ArgumentList @("--debug","--rewrite-demo") -WorkingDirectory $repo -RedirectStandardOutput "$repo\tests\_out.txt" -RedirectStandardError "$repo\tests\_err.txt" -PassThru
Start-Sleep -Milliseconds 300
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$idx = 0
$csv = New-Object System.Collections.Generic.List[string]
$csv.Add("frame,ms")
while ($sw.ElapsedMilliseconds -lt 4200) {
    $h = [WinApi3]::Find("Tiro rewrite demo")
    if ($h -ne [IntPtr]::Zero) {
        $r = New-Object WinApi3+RECT
        [void][WinApi3]::GetWindowRect($h, [ref]$r)
        $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
        if ($w -gt 40 -and $ht -gt 40) {
            $bmp = New-Object System.Drawing.Bitmap($w, $ht)
            $gfx = [System.Drawing.Graphics]::FromImage($bmp)
            $gfx.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
            $bmp.Save(("$outDir\frame_{0:d4}.png" -f $idx))
            $gfx.Dispose(); $bmp.Dispose()
            $csv.Add("$idx,$($sw.ElapsedMilliseconds)")
            $idx++
        }
    }
    Start-Sleep -Milliseconds 33
}
$csv | Set-Content "$outDir\frames.csv"
Write-Output "mode=$mode captured=$idx"
