#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Creates (or refreshes) a Hyper-V virtual machine that boots Nocturne OS.

.DESCRIPTION
    Generation 2 (the default for a new VM) is the native Hyper-V machine: UEFI, VMBus and only
    synthetic devices, which Nocturne drives (keyboard, mouse, SCSI disks, network, heartbeat,
    shutdown, time sync) - plus the Enhanced Session: VMConnect talks RDP to Nocturne over a
    Hyper-V socket, so the desktop takes the size of the window. For a Gen 2 VM this script:
      * copies build\nocturne.vhdx to .\hyperv\<Name>-boot.vhdx (so rebuilding never fights a locked disk),
      * creates the VM with static memory, one CPU, no checkpoints, Secure Boot off (the boot loader
        is not signed) and a synthetic network adapter on the "Default Switch" for NAT internet access,
      * attaches the boot disk on SCSI 0:0 and makes it the first boot device,
      * attaches a persistent 2 GiB data disk (.\hyperv\<Name>-data.vhdx, FAT32 "NOCTDATA") on SCSI 0:1,
        which Nocturne mounts at /data. It is created once and never replaced by -Update or -Remove,
      * selects the Hyper-V socket transport for the Enhanced Session and turns Enhanced Session Mode
        on for the host if it is off,
      * optionally writes an AI provider key into /data/etc/agent.conf (-ApiKeyFile, for the agent program),
      * routes COM1 to the named pipe \\.\pipe\<name>-com1 (kernel log),
      * starts the VM and opens a VMConnect window.

    Generation 1 (-Generation 1) emulates a legacy PC: build\nocturne.vhd on IDE 0:0, the data disk
    on IDE 0:1 and a legacy network adapter (an emulated DEC 21140).

    -Update keeps the generation of the existing VM.

.EXAMPLE
    .\hyperv.ps1                 # create and start the Gen 2 VM "Nocturne"
.EXAMPLE
    .\hyperv.ps1 -Name Nocturne-G1 -Generation 1   # a Gen 1 VM instead
.EXAMPLE
    .\hyperv.ps1 -Update         # after rebuilding: turn the VM off, copy the new disk, start it again
.EXAMPLE
    .\hyperv.ps1 -Remove         # delete the VM (and its copied disk)
.EXAMPLE
    .\hyperv.ps1 -Iso            # boot from build\nocturne.iso on the virtual DVD drive instead of the disk
.EXAMPLE
    .\hyperv.ps1 -Update -ApiKeyFile C:\keys\llm.txt   # also store an API key for `agent` on the data disk
.EXAMPLE
    .\hyperv.ps1 -Remove -DeleteData   # delete the VM, its disk *and* the data disk
#>
param(
    [string]$Name = "Nocturne",
    [ValidateSet(1, 2)][int]$Generation = 2,
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
$SrcIso = Join-Path $Root "build\nocturne.iso"
$IsoCopy = Join-Path $VmDir "$Name.iso"
$SrcData = Join-Path $Root "build\data-blank.vhdx"
$DataVhd = Join-Path $VmDir "$Name-data.vhdx"
$Pipe = "\\.\pipe\$($Name.ToLower())-com1"

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
if ($existing) { $Generation = $existing.Generation }

# Gen 1 boots the fixed-size VHD on IDE; Gen 2 boots the VHDX on SCSI
if ($Generation -eq 1) {
    $SrcDisk = Join-Path $Root "build\nocturne.vhd"
    $Disk = Join-Path $VmDir "$Name.vhd"
    $Bus = "IDE"
}
else {
    $SrcDisk = Join-Path $Root "build\nocturne.vhdx"
    $Disk = Join-Path $VmDir "$Name-boot.vhdx"
    $Bus = "SCSI"
}

if ($Remove) {
    if ($existing) {
        Stop-IfRunning $existing
        Remove-VM -VM $existing -Force
        Say "removed VM '$Name'"
    }
    # the configuration folder New-VM made, once Hyper-V has emptied it
    $cfg = Join-Path $VmDir $Name
    if ((Test-Path $cfg) -and -not (Get-ChildItem $cfg -Recurse -File)) { Remove-Item $cfg -Recurse -Force }
    foreach ($f in @($Disk, $IsoCopy)) { if (Test-Path $f) { Remove-Item $f -Force } }
    if ($DeleteData -and (Test-Path $DataVhd)) {
        Remove-Item $DataVhd -Force
        Say "deleted the data disk"
    }
    elseif (Test-Path $DataVhd) {
        Say "kept the data disk $DataVhd (pass -DeleteData to delete it too)"
    }
    return
}

if (-not (Test-Path $SrcDisk)) {
    throw "$SrcDisk not found. Build first:  powershell -ExecutionPolicy Bypass -File build.ps1"
}
New-Item -ItemType Directory -Force $VmDir | Out-Null

function Copy-Media {
    Say "copying disk image -> $Disk"
    Copy-Item $SrcDisk $Disk -Force
    if ($Iso) {
        if (-not (Test-Path $SrcIso)) { throw "build\nocturne.iso not found" }
        Copy-Item $SrcIso $IsoCopy -Force
    }
}

function Test-Switch {
    if (Get-VMSwitch -Name $Switch -ErrorAction SilentlyContinue) { return $true }
    Write-Warning "virtual switch '$Switch' not found; the VM gets no network. Pass -Switch <name> to use another one."
    return $false
}

function Add-Nic($vm) {
    if ($NoNetwork) { return }
    if ($Generation -eq 1) {
        if (Get-VMNetworkAdapter -VM $vm | Where-Object { $_.IsLegacy }) { return }
        if (-not (Test-Switch)) { return }
        Say "adding a legacy network adapter on '$Switch'"
        Add-VMNetworkAdapter -VM $vm -IsLegacy $true -SwitchName $Switch
    }
    else {
        if (Get-VMNetworkAdapter -VM $vm) { return }
        if (-not (Test-Switch)) { return }
        Say "adding a network adapter on '$Switch'"
        Add-VMNetworkAdapter -VM $vm -SwitchName $Switch
    }
}

function Add-DataDisk($vmName) {
    if (-not (Test-Path $DataVhd)) {
        if (-not (Test-Path $SrcData)) { throw "build\data-blank.vhdx not found; rebuild first" }
        Say "creating the persistent data disk -> $DataVhd"
        Copy-Item $SrcData $DataVhd
    }
    if (-not (Get-VMHardDiskDrive -VMName $vmName | Where-Object { $_.Path -eq $DataVhd })) {
        Say "attaching the data disk on $Bus 0:1"
        Add-VMHardDiskDrive -VMName $vmName -ControllerType $Bus -ControllerNumber 0 -ControllerLocation 1 -Path $DataVhd
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

# Gen 2: the DVD drive (for -Iso) and the boot order
function Set-Gen2Boot($vm) {
    $first = Get-VMHardDiskDrive -VM $vm | Where-Object { $_.Path -eq $Disk }
    if ($Iso) {
        $dvd = Get-VMDvdDrive -VM $vm | Select-Object -First 1
        if (-not $dvd) { $dvd = Add-VMDvdDrive -VM $vm -Path $IsoCopy -Passthru }
        else { Set-VMDvdDrive -VMDvdDrive $dvd -Path $IsoCopy }
        $first = $dvd
    }
    Set-VMFirmware -VM $vm -FirstBootDevice $first
}

if ($existing -and $Update) {
    Stop-IfRunning $existing
    Add-Nic $existing
    Copy-Media
    Add-DataDisk $Name
    if ($Iso -and $Generation -eq 1) { Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $IsoCopy }
    if ($Generation -eq 2) { Set-Gen2Boot $existing }
}
elseif ($existing) {
    throw "A VM called '$Name' already exists. Use -Update to refresh its disk, or -Remove to delete it."
}
else {
    Copy-Media
    Say "creating Generation $Generation VM '$Name' ($MemoryMB MB)"
    $vm = New-VM -Name $Name -Generation $Generation -MemoryStartupBytes ($MemoryMB * 1MB) -VHDPath $Disk -Path $VmDir
    Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
    Set-VMProcessor -VM $vm -Count 1
    Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter
    Add-Nic $vm
    try { Set-VM -VM $vm -AutomaticCheckpointsEnabled $false -CheckpointType Disabled } catch { }
    Set-VM -VM $vm -AutomaticStopAction TurnOff
    # kernel log on COM1 -> named pipe (read it with scripts\hv-serial.ps1, PuTTY or any pipe client)
    Set-VMComPort -VM $vm -Number 1 -Path $Pipe
    Add-DataDisk $Name
    if ($Generation -eq 1) {
        if ($Iso) {
            Set-VMDvdDrive -VMName $Name -ControllerNumber 1 -ControllerLocation 0 -Path $IsoCopy
            Set-VMBios -VM $vm -StartupOrder @("CD", "IDE", "LegacyNetworkAdapter", "Floppy")
        }
        else {
            Set-VMBios -VM $vm -StartupOrder @("IDE", "CD", "LegacyNetworkAdapter", "Floppy")
        }
    }
    else {
        Set-VMFirmware -VM $vm -EnableSecureBoot Off
        Set-Gen2Boot $vm
        # VMConnect's Enhanced Session: RDP to Nocturne over a Hyper-V socket
        Set-VM -VM $vm -EnhancedSessionTransportType HvSocket
    }
}

if ($Generation -eq 2 -and -not (Get-VMHost).EnableEnhancedSessionMode) {
    Say "turning on Enhanced Session Mode for this Hyper-V host"
    Set-VMHost -EnableEnhancedSessionMode $true
}

if ($ApiKeyFile) { Set-AgentKey }

if (-not $NoStart) {
    Say "starting '$Name'"
    Start-VM -Name $Name
    Start-Process vmconnect.exe -ArgumentList "localhost", "`"$Name`""
    if ($Generation -eq 2) {
        Say "VMConnect asks for a display size: that is the Enhanced Session, the desktop takes that size."
    }
    else {
        Say "click inside the VM window to capture the mouse; Ctrl+Alt+Left releases it."
    }
}
