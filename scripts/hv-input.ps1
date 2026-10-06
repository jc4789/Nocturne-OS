<#
.SYNOPSIS
    Send keyboard and mouse input to a running Hyper-V VM through the host's management API
    (Msvm_Keyboard / Msvm_SyntheticMouse), without VMConnect. Steps run in order.
.EXAMPLE
    .\scripts\hv-input.ps1 -Name Nocturne-G2 -Steps "text:neofetch","key:13","sleep:1","move:500,300","click:1"
    # text:STR  type ASCII text        key:VK      type a Windows virtual-key code (13 = Enter)
    # move:X,Y  absolute pointer (px)  click:N     click button N (1 left, 2 right, 3 middle)
    # down:N / up:N  hold or release a mouse button   sleep:S  wait S seconds
    # ctrl:VK   type VK with Ctrl held  alt:VK   with Alt held
#>
param(
    [string]$Name = "Nocturne-G2",
    [string[]]$Steps
)
$ErrorActionPreference = "Stop"
$ns = "root\virtualization\v2"
$vm = Get-CimInstance -Namespace $ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Name'"
if (-not $vm) { throw "VM '$Name' not found" }
$kbd = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_Keyboard | Select-Object -First 1
$mouse = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_SyntheticMouse | Select-Object -First 1

function Call($obj, $method, $arguments) {
    if (-not $obj) { throw "no device for $method" }
    $r = Invoke-CimMethod -InputObject $obj -MethodName $method -Arguments $arguments
    if ($r.ReturnValue -ne 0) { Write-Warning "$method returned $($r.ReturnValue)" }
}

foreach ($s in $Steps) {
    $kind, $arg = $s -split ':', 2
    switch ($kind) {
        'text'  { Call $kbd TypeText @{ asciiText = $arg } }
        'key'   { Call $kbd TypeKey @{ keyCode = [uint32]$arg } }
        'ctrl'  { Call $kbd PressKey @{ keyCode = [uint32]17 }; Call $kbd TypeKey @{ keyCode = [uint32]$arg }; Call $kbd ReleaseKey @{ keyCode = [uint32]17 } }
        'alt'   { Call $kbd PressKey @{ keyCode = [uint32]18 }; Call $kbd TypeKey @{ keyCode = [uint32]$arg }; Call $kbd ReleaseKey @{ keyCode = [uint32]18 } }
        'move'  { $x, $y = $arg -split ','; Call $mouse SetAbsolutePosition @{ horizontalPosition = [uint32]$x; verticalPosition = [uint32]$y } }
        'click' { Call $mouse ClickButton @{ buttonIndex = [uint16]$arg } }
        'down'  { Call $mouse SetButton @{ buttonIndex = [uint16]$arg; isPressed = $true } }
        'up'    { Call $mouse SetButton @{ buttonIndex = [uint16]$arg; isPressed = $false } }
        'sleep' { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
        default { throw "unknown step '$s'" }
    }
}
