#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Creates (or refreshes) a Hyper-V virtual machine that boots Nocturne OS.

.DESCRIPTION
    Nocturne talks to legacy PC hardware (PS/2 keyboard and mouse, IDE disk, VBE framebuffer),
    so it needs a *Generation 1* VM. This script:
      * copies build\nocturne.vhd into .\hyperv\ (so rebuilding never fights a locked disk),
      * creates a Gen1 VM with static memory, one CPU, no checkpoints and a *legacy* network adapter
        (an emulated DEC 21140, which Nocturne drives) on the "Default Switch" for NAT internet access,
      * attaches the disk to IDE 0:0 and makes IDE the first boot device,
      * attaches a persistent 2 GiB data disk (.\hyperv\<Name>-data.vhdx, FAT32 "NOCTDATA") on IDE 0:1,
        which Nocturne mounts at /data. It is created once and never replaced by -Update or -Remove,
      * optionally writes an AI provider key into /data/etc/agent.conf (-ApiKeyFile, for the agent program),
      * routes COM1 to the named pipe \\.\pipe\nocturne-com1 (kernel log),
      * starts the VM and opens a VMConnect window.

.EXAMPLE
    .\hyperv.ps1                 # create and start the VM "Nocturne"
.EXAMPLE
    .\hyperv.ps1 -Update         # after rebuilding: turn the VM off, copy the new disk, start it again
.EXAMPLE
    .\hyperv.ps1 -Remove         # delete the VM (and its copied disk)
.EXAMPLE
    .\hyperv.ps1 -Iso            # boot from build\nocturne.iso on the virtual DVD drive instead of the VHD
.EXAMPLE
    .\hyperv.ps1 -Update -ApiKeyFile C:\keys\llm.txt   # also store an API key for `agent` on the data disk
.EXAMPLE
    .\hyperv.ps1 -Remove -DeleteData   # delete the VM, its disk *and* the data disk
#>
param(
    [string]$Name = "Nocturne",
    [int]$MemoryMB = 512,
    [switch]$Update,
    [switch]$Remove,
    [switch]$Iso,
    [switch]$NoStart,
    [switch]$NoNetwork,
    [string]$Switch = "Default Switch",
    [string]$ApiKeyFile,
    [string]$Endpoint = "https://hyper.charm.land/v1",
    [string]$Model = "glm-5.3-flash",
    [switch]$DeleteData
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$VmDir = Join-Path $Root "hyperv"
$SrcVhd = Join-Path $Root "build\nocturne.vhd"
$SrcIso = Join-Path $Root "build\nocturne.iso"
$Vhd = Join-Path $VmDir "$Name.vhd"
$IsoCopy = Join-Path $VmDir "$Name.iso"
$SrcData = Join-Path $Root "build\data-blank.vhdx"
$DataVhd = Join-Path $VmDir "$Name-data.vhdx"

function Say($msg) { Write-Host "[nocturne] $msg" -ForegroundColor Magenta }

if (-not (Get-Command Get-VM -ErrorAction SilentlyContinue)) {
    throw "The Hyper-V PowerShell module is not available. Enable 'Hyper-V Module for Windows PowerShell' in Windows Features."
}

function Stop-IfRunning($vm) {
    if ($vm -and $vm.State -ne 'Off') {
        Say "turning off '$($vm.Name)'"
        Stop-VM -VM $vm -TurnOff -Force
    }
}

$existing = Get-VM -Name $Name -ErrorAction SilentlyContinue

if ($Remove) {
    if ($existing) {
        Stop-IfRunning $existing
        Remove-VM -VM $existing -Force
        Say "removed VM '$Name'"
    }
    foreach ($f in @($Vhd, $IsoCopy)) { if (Test-Path $f) { Remove-Item $f -Force } }
    if ($DeleteData -and (Test-Path $DataVhd)) {
        Remove-Item $DataVhd -Force
        Say "deleted the data disk"
    }
    elseif (Test-Path $DataVhd) {
        Say "kept the data disk $DataVhd (pass -DeleteData to delete it too)"
    }
    return
}

if (-not (Test-Path $SrcVhd)) {
    throw "build\nocturne.vhd not found. Build first:  powershell -ExecutionPolicy Bypass -File build.ps1"
}
New-Item -ItemType Directory -Force $VmDir | Out-Null

function Copy-Media {
    Say "copying disk image -> $Vhd"
    Copy-Item $SrcVhd $Vhd -Force
    if ($Iso) {
        if (-not (Test-Path $SrcIso)) { throw "build\nocturne.iso not found" }
        Copy-Item $SrcIso $IsoCopy -Force
    }
}

function Add-LegacyNic($vm) {
    if ($NoNetwork) { return }
    if (Get-VMNetworkAdapter -VM $vm | Where-Object { $_.IsLegacy }) { return }
    if (-not (Get-VMSwitch -Name $Switch -ErrorAction SilentlyContinue)) {
        Write-Warning "virtual switch '$Switch' not found; the VM gets no network. Pass -Switch <name> to use another one."
        return
    }
    Say "adding a legacy network adapter on '$Switch'"
    Add-VMNetworkAdapter -VM $vm -IsLegacy $true -SwitchName $Switch
}

function Add-DataDisk($vmName) {
    if (-not (Test-Path $DataVhd)) {
        if (-not (Test-Path $SrcData)) { throw "build\data-blank.vhdx not found; rebuild first" }
        Say "creating the persistent data disk -> $DataVhd"
        Copy-Item $SrcData $DataVhd
    }
    $att = Get-VMHardDiskDrive -VMName $vmName -ControllerType IDE -ControllerNumber 0 -ControllerLocation 1
    if (-not $att) {
        Say "attaching the data disk on IDE 0:1"
        Add-VMHardDiskDrive -VMName $vmName -ControllerType IDE -ControllerNumber 0 -ControllerLocation 1 -Path $DataVhd
    }
}

# Write /etc/agent.conf onto the data disk (the VM must be off). The key is read from a text file that
# either holds just the key or has a line like "api key: <key>"; it is never printed.
function Set-AgentKey {
    $text = Get-Content -Raw $ApiKeyFile
    $key = $null
    if ($text -match '(?im)^\s*api[ _-]?key\s*[:=]\s*(\S+)') { $key = $Matches[1] }
    elseif ($text.Trim() -notmatch '\s') { $key = $text.Trim() }
    if (-not $key) { throw "could not find the key in $ApiKeyFile (expected a line like 'api key: ...')" }
    $disk = Mount-VHD -Path $DataVhd -Passthru | Get-Disk
    try {
        $part = Get-Partition -DiskNumber $disk.Number | Select-Object -First 1
        if (-not $part.DriveLetter) {
            $part | Add-PartitionAccessPath -AssignDriveLetter
            $part = Get-Partition -DiskNumber $disk.Number | Select-Object -First 1
        }
        $etc = "$($part.DriveLetter):\etc"
        New-Item -ItemType Directory -Force $etc | Out-Null
        $conf = "# written by hyperv.ps1 -ApiKeyFile`nendpoint=$Endpoint`nmodel=$Model`napi_key=$key`n"
        [System.IO.File]::WriteAllText("$etc\agent.conf", $conf)
        Say "stored the API key in /data/etc/agent.conf (endpoint $Endpoint, model $Model)"
    }
    finally {
        Dismount-VHD -Path $DataVhd
    }
}

if ($existing -and $Update) {
    Stop-IfRunning $existing
    Add-LegacyNic $existing
    Copy-Media
    Add-DataDisk $Name
    if ($Iso) { Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $IsoCopy }
}
elseif ($existing) {
    throw "A VM called '$Name' already exists. Use -Update to refresh its disk, or -Remove to delete it."
}
else {
    Copy-Media
    Say "creating Generation 1 VM '$Name' ($MemoryMB MB)"
    $vm = New-VM -Name $Name -Generation 1 -MemoryStartupBytes ($MemoryMB * 1MB) -VHDPath $Vhd -Path $VmDir
    Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
    Set-VMProcessor -VM $vm -Count 1
    # Nocturne has no Hyper-V (VMBus) drivers: swap the synthetic NIC for a legacy one, disable checkpoints
    Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter
    Add-LegacyNic $vm
    try { Set-VM -VM $vm -AutomaticCheckpointsEnabled $false -CheckpointType Disabled } catch { }
    Set-VM -VM $vm -AutomaticStopAction TurnOff
    # kernel log on COM1 -> named pipe (read it with PuTTY or any pipe client)
    Set-VMComPort -VM $vm -Number 1 -Path "\\.\pipe\nocturne-com1"
    Add-DataDisk $Name
    if ($Iso) {
        Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $IsoCopy
        Set-VMBios -VM $vm -StartupOrder @("CD", "IDE", "LegacyNetworkAdapter", "Floppy")
    }
    else {
        Set-VMBios -VM $vm -StartupOrder @("IDE", "CD", "LegacyNetworkAdapter", "Floppy")
    }
}

if ($ApiKeyFile) { Set-AgentKey }

if (-not $NoStart) {
    Say "starting '$Name'"
    Start-VM -Name $Name
    Start-Process vmconnect.exe -ArgumentList "localhost", "`"$Name`""
    Say "click inside the VM window to capture the mouse; Ctrl+Alt+Left releases it."
}
