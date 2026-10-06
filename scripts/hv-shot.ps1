<#
.SYNOPSIS
    Save a screenshot of a running Hyper-V VM's console as a PNG (no VMConnect needed).
.EXAMPLE
    .\scripts\hv-shot.ps1 -Name Nocturne-G2 -Out build\g2.png
#>
param(
    [string]$Name = "Nocturne",
    [string]$Out = "build\hv-shot.png",
    [int]$Width = 0,
    [int]$Height = 0
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$ns = "root\virtualization\v2"
$vm = Get-CimInstance -Namespace $ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Name'"
if (-not $vm) { throw "VM '$Name' not found" }
$vssd = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VirtualSystemSettingData |
    Where-Object { $_.VirtualSystemType -eq "Microsoft:Hyper-V:System:Realized" }
if ($Width -le 0 -or $Height -le 0) {
    # The console's current resolution, from the video head.
    $head = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VideoHead | Select-Object -First 1
    $Width = [int]$head.CurrentHorizontalResolution
    $Height = [int]$head.CurrentVerticalResolution
    if ($Width -le 0) { $Width = 1024; $Height = 768 }
}
$svc = Get-CimInstance -Namespace $ns -ClassName Msvm_VirtualSystemManagementService
$r = Invoke-CimMethod -InputObject $svc -MethodName GetVirtualSystemThumbnailImage -Arguments @{
    TargetSystem = $vssd; WidthPixels = [uint16]$Width; HeightPixels = [uint16]$Height
}
if ($r.ReturnValue -ne 0) { throw "GetVirtualSystemThumbnailImage failed: $($r.ReturnValue)" }
$data = [byte[]]$r.ImageData

# The image is RGB565, row by row.
$bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format16bppRgb565)
$rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
for ($y = 0; $y -lt $Height; $y++) {
    [System.Runtime.InteropServices.Marshal]::Copy($data, $y * $Width * 2, [IntPtr]($bd.Scan0.ToInt64() + $y * $bd.Stride), $Width * 2)
}
$bmp.UnlockBits($bd)
$full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $full (${Width}x$Height)"
