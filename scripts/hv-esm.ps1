<#
.SYNOPSIS
    Open an enhanced session to a VM with VMConnect, press Connect in its display dialog, take a
    screenshot of the window after a delay and (unless -Keep) close it again.
.EXAMPLE
    .\scripts\hv-esm.ps1 -Name Nocturne-G2 -Seconds 10 -Out build\hv\esm.png
#>
param(
    [string]$Name = "Nocturne-G2",
    [int]$Seconds = 10,
    [string]$Out = "build\hv\esm.png",
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
if (-not ("EsmWin" -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class EsmWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
}
$A = [System.Windows.Automation.AutomationElement]
Get-Process vmconnect -ErrorAction SilentlyContinue | Stop-Process -Force
$p = Start-Process vmconnect.exe -ArgumentList "localhost", $Name -PassThru
$cond = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
$win = $null
for ($i = 0; $i -lt 40 -and -not $win; $i++) {
    Start-Sleep -Milliseconds 250
    $win = $A::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
}
if (-not $win) { throw "no VMConnect window" }
# The display dialog appears only when the guest offers an enhanced session and VMConnect has no
# saved settings for the VM; it shows up within about 15 seconds. Walking VMConnect's whole UI tree
# is slow, so look at buttons only and give up after a fixed time.
$pressed = $false
$isButton = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)
$deadline = (Get-Date).AddSeconds(25)
while (-not $pressed -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $all = $A::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
    foreach ($w in $all) {
        $buttons = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants, $isButton) |
            Where-Object { $_.Current.Name -match '^(接続|Connect)\(&?N\)$' }
        foreach ($b in $buttons) {
            $pat = $null
            if ($b.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) {
                $pat.Invoke()
                $pressed = $true
                break
            }
        }
        if ($pressed) { break }
    }
}
if ($pressed) { Write-Host "enhanced session: Connect pressed" }
else { Write-Host "no display dialog (VMConnect reused saved settings, or the session is basic)" }
Start-Sleep -Seconds $Seconds
$p.Refresh()
$h = $p.MainWindowHandle
$r = New-Object EsmWin+RECT
[void][EsmWin]::GetWindowRect($h, [ref]$r)
# capture the window itself, not the screen: other windows may be on top, and the
# desktop belongs to the user, so it is never brought to the front or given input
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[void][EsmWin]::PrintWindow($h, $hdc, 2) # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$bmp.Save((Join-Path (Resolve-Path (Split-Path $Out)) (Split-Path $Out -Leaf)), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "saved $Out ($($bmp.Width)x$($bmp.Height))"
if (-not $Keep) { Stop-Process -Id $p.Id -Force }
