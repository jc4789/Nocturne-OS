#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Creates (or refreshes) a Hyper-V virtual machine that boots Nocturne OS.

.DESCRIPTION
    Nocturne has one disk: its boot part and drive C:, with the system (C:\Nocturne), the apps
    (C:\Programs) and the user's files (C:\Users). The VM's disk is .\hyperv\<Name>.vhdx, the VM's
    own: -Update replaces only the system on it (the boot part, C:\Nocturne and the desktop apps'
    folders, scripts\mkimage.sh --update), so the user's files and the apps the user put in
    C:\Programs stay. A new VM's disk is a copy of build\nocturne.img; one left by -Remove is taken
    up again.

    The VM is a Generation 2 machine, the native Hyper-V one: UEFI, VMBus and only synthetic
    devices, which Nocturne drives (keyboard, mouse, SCSI disks, network, heartbeat, shutdown, time
    sync) - plus the Enhanced Session: VMConnect talks RDP to Nocturne over a Hyper-V socket, so the
    desktop takes the size of the window. A Generation 1 VM is not one Nocturne boots on; the
    script stops at one. This script:
      * writes the disk (above) and attaches it on SCSI 0:0 as the first boot device,
      * creates the VM with static memory (-MemoryMB, 4096), four CPUs (-Cpus), no checkpoints, Secure Boot off (the boot loader
        is not signed) and a synthetic network adapter on the "Default Switch" for NAT internet access,
      * selects the Hyper-V socket transport for the Enhanced Session and turns Enhanced Session Mode
        on for the host if it is off,
      * optionally stores an AI provider key for the agent program in the user's
        AppData\Agent\agent.conf (-ApiKeyFile, scripts\setkey.py),
      * routes COM1 to the named pipe \\.\pipe\<name>-com1 (kernel log),
      * starts the VM and opens a VMConnect window.

    -Update rewrites the disk through a raw copy next to it (.\hyperv\<Name>-work.img, sparse), so
    the folder needs room for twice what the disk holds (its file's size, not the size the guest
    sees).

.EXAMPLE
    .\hyperv.ps1                 # create and start the VM "Nocturne"
.EXAMPLE
    .\hyperv.ps1 -Update         # after rebuilding: turn the VM off, put the new system on its disk, start it again
.EXAMPLE
    .\hyperv.ps1 -Remove         # delete the VM; its disk, with the user's files, stays
.EXAMPLE
    .\hyperv.ps1 -Iso            # boot the live system build\nocturne.iso on the virtual DVD drive instead of the disk
                                 # (its C: is in memory and its changes go with it)
.EXAMPLE
    .\hyperv.ps1 -Update -ApiKeyFile C:\keys\llm.txt   # also store an API key for `agent`
.EXAMPLE
    .\hyperv.ps1 -Remove -DeleteDisk   # delete the VM and its disk
#>
param(
    [string]$Name = "Nocturne",
    # What the browser needs for today's sites, and the CPUs its parallel work spreads over: with one
    # CPU and 512 MB a video page runs out of memory and per-frame WebGL runs at a third of its speed.
    [int]$MemoryMB = 4096,
    [ValidateRange(1, 240)][int]$Cpus = 4,
    [switch]$Update,
    [switch]$Remove,
    [switch]$Iso,
    [switch]$NoStart,
    [switch]$NoNetwork,
    [string]$Switch = "Default Switch",
    [string]$ApiKeyFile,
    [string]$Endpoint = "https://hyper.charm.land/v1",
    [string]$Model = "glm-5.3-flash",
    [switch]$DeleteDisk
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$VmDir = Join-Path $Root "hyperv"
$Img = Join-Path $Root "build\nocturne.img"
$SrcIso = Join-Path $Root "build\nocturne.iso"
$IsoCopy = Join-Path $VmDir "$Name.iso"
$OldDataVhd = Join-Path $VmDir "$Name-data.vhdx" # an older Nocturne's second disk
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
if ($existing -and $existing.Generation -ne 2) {
    throw "VM '$Name' is a Generation $($existing.Generation) VM; Nocturne boots only on Generation 2. Delete it in Hyper-V Manager (its disk stays on the host) and run this script again."
}

# the VHDX on SCSI 0:0
$Disk = Join-Path $VmDir "$Name.vhdx"
if ($existing) {
    $drive = Get-VMHardDiskDrive -VM $existing -ControllerType SCSI -ControllerNumber 0 -ControllerLocation 0
    if ($drive) { $Disk = $drive.Path }
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
    if (Test-Path $IsoCopy) { Remove-Item $IsoCopy -Force }
    if ($DeleteDisk -and (Test-Path $Disk)) {
        Remove-Item $Disk -Force
        Say "deleted the disk $Disk"
    }
    elseif (Test-Path $Disk) {
        Say "kept the disk $Disk with the user's files: a new VM '$Name' takes it up again (-DeleteDisk deletes it too)"
    }
    return
}

# A command in the build's own MSYS2 shell (its qemu-img and mtools, build\host\store.exe), run in
# the project folder. Paths go in NOCTURNE_* environment variables: Windows PowerShell 5.1 drops
# the quotes around native arguments, and the project path has spaces.
function Invoke-Build([string]$Command, [switch]$Test) {
    $env:MSYSTEM = "UCRT64"
    $env:CHERE_INVOKING = "1"
    $env:MSYS2_PATH_TYPE = "inherit"
    Push-Location $Root
    try {
        & "$Root\tools\msys64\usr\bin\bash.exe" -lc $Command | Out-Host
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($Test) { return $code -eq 0 }
    if ($code) { throw "failed ($code): $Command" }
}

# The VM's disk with the built system on it: its own disk (the user's files stay) or, when it has
# none, a copy of build\nocturne.img. A disk with no C: volume is an older Nocturne's, which kept
# the user's files on a second disk: it gets the new system whole.
function Write-Disk {
    foreach ($tool in @("build\kernel.elf", "build\host\store.exe")) {
        if (-not (Test-Path (Join-Path $Root $tool))) { throw "$tool not found. Build first:  powershell -ExecutionPolicy Bypass -File build.ps1" }
    }
    $fresh = -not (Test-Path $Disk)
    # what the disk holds: the raw copy is a sparse file, and a dynamic VHDX's file is that much
    $size = if ($fresh) { (Get-Item $Img -ErrorAction SilentlyContinue).Length } else { (Get-VHD -Path $Disk).FileSize }
    $free = (Get-Item $VmDir).PSDrive.Free
    if ($size -and $free -lt 2 * $size) {
        throw "$VmDir has $([math]::Round($free / 1GB, 1)) GiB free; rewriting the disk needs $([math]::Round(2 * $size / 1GB, 1)) GiB"
    }
    $env:NOCTURNE_DISK = $Disk
    $env:NOCTURNE_WORK = "hyperv/$Name-work.img"
    $env:NOCTURNE_NEW = "hyperv/$Name-new.vhdx"
    $work = Join-Path $VmDir "$Name-work.img"
    $new = Join-Path $VmDir "$Name-new.vhdx"
    try {
        if (-not $fresh) {
            Say "reading $Disk"
            Invoke-Build 'qemu-img convert -O raw "$NOCTURNE_DISK" "$NOCTURNE_WORK"'
            if (-not (Invoke-Build 'build/host/store.exe list "$NOCTURNE_WORK" C:/ >/dev/null 2>&1' -Test)) {
                Say "$Disk is an older Nocturne's disk, with no drive C:; it gets the new system whole"
                $fresh = $true
            }
        }
        if ($fresh) {
            # `make qemu-image` refreshes only the raw disk; never hand a VM one older than the kernel
            if (-not (Test-Path $Img) -or (Get-Item (Join-Path $Root "build\kernel.elf")).LastWriteTime -gt (Get-Item $Img).LastWriteTime) {
                throw "build\nocturne.img is missing or older than build\kernel.elf. Build first:  powershell -ExecutionPolicy Bypass -File build.ps1"
            }
            Say "copying build\nocturne.img"
            Copy-Item $Img $work -Force
        }
        else {
            Invoke-Build 'bash scripts/mkimage.sh --update "$NOCTURNE_WORK"'
        }
        if ($ApiKeyFile) {
            $env:NOCTURNE_KEYFILE = (Resolve-Path $ApiKeyFile).Path
            $env:NOCTURNE_ENDPOINT = $Endpoint
            $env:NOCTURNE_MODEL = $Model
            Invoke-Build 'python scripts/setkey.py "$NOCTURNE_KEYFILE" --endpoint "$NOCTURNE_ENDPOINT" --model "$NOCTURNE_MODEL" --img "$NOCTURNE_WORK"'
        }
        Invoke-Build 'bash scripts/mkimage.sh --hyperv "$NOCTURNE_WORK" "$NOCTURNE_NEW"'
        # copied over the old disk, which keeps the access Hyper-V gave the VM to it
        Say "writing $Disk"
        Copy-Item $new $Disk -Force
    }
    finally {
        foreach ($f in @($work, $new)) { if (Test-Path $f) { Remove-Item $f -Force } }
    }
}

function Copy-Iso {
    if (-not $Iso) { return }
    if (-not (Test-Path $SrcIso)) { throw "build\nocturne.iso not found" }
    Copy-Item $SrcIso $IsoCopy -Force
}

function Test-Switch {
    if (Get-VMSwitch -Name $Switch -ErrorAction SilentlyContinue) { return $true }
    Write-Warning "virtual switch '$Switch' not found; the VM gets no network. Pass -Switch <name> to use another one."
    return $false
}

function Add-Nic($vm) {
    if ($NoNetwork) { return }
    if (Get-VMNetworkAdapter -VM $vm) { return }
    if (-not (Test-Switch)) { return }
    Say "adding a network adapter on '$Switch'"
    Add-VMNetworkAdapter -VM $vm -SwitchName $Switch
}

# the DVD drive (for -Iso) and the boot order
function Set-Boot($vm) {
    $first = Get-VMHardDiskDrive -VM $vm | Where-Object { $_.Path -eq $Disk }
    if ($Iso) {
        $dvd = Get-VMDvdDrive -VM $vm | Select-Object -First 1
        if (-not $dvd) { $dvd = Add-VMDvdDrive -VM $vm -Path $IsoCopy -Passthru }
        else { Set-VMDvdDrive -VMDvdDrive $dvd -Path $IsoCopy }
        $first = $dvd
    }
    Set-VMFirmware -VM $vm -FirstBootDevice $first
}

New-Item -ItemType Directory -Force $VmDir | Out-Null

if ($existing -and $Update) {
    Stop-IfRunning $existing
    # an older Nocturne's second disk: Nocturne keeps everything on its one disk now
    $old = Get-VMHardDiskDrive -VM $existing | Where-Object { $_.Path -eq $OldDataVhd }
    if ($old) {
        Remove-VMHardDiskDrive -VMHardDiskDrive $old
        Say "detached the old data disk; Nocturne no longer uses it. It stays on the host: $OldDataVhd"
    }
    Add-Nic $existing
    Write-Disk
    Copy-Iso
    if ($Iso -and $existing.MemoryStartup -lt $MemoryMB * 1MB) {
        Say "giving '$Name' $MemoryMB MB for the ISO's C:"
        Set-VMMemory -VM $existing -StartupBytes ($MemoryMB * 1MB)
    }
    Set-Boot $existing
}
elseif ($existing) {
    throw "A VM called '$Name' already exists. Use -Update to put the new system on its disk, or -Remove to delete it."
}
else {
    Write-Disk
    Copy-Iso
    Say "creating VM '$Name' ($MemoryMB MB, $Cpus CPUs)"
    $vm = New-VM -Name $Name -Generation 2 -MemoryStartupBytes ($MemoryMB * 1MB) -VHDPath $Disk -Path $VmDir
    Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
    Set-VMProcessor -VM $vm -Count ([Math]::Min($Cpus, [Environment]::ProcessorCount))
    Get-VMNetworkAdapter -VM $vm | Remove-VMNetworkAdapter
    Add-Nic $vm
    try { Set-VM -VM $vm -AutomaticCheckpointsEnabled $false -CheckpointType Disabled } catch { }
    Set-VM -VM $vm -AutomaticStopAction TurnOff
    # kernel log on COM1 -> named pipe (read it with scripts\hv-serial.ps1, PuTTY or any pipe client)
    Set-VMComPort -VM $vm -Number 1 -Path $Pipe
    Set-VMFirmware -VM $vm -EnableSecureBoot Off
    Set-Boot $vm
    # VMConnect's Enhanced Session: RDP to Nocturne over a Hyper-V socket
    Set-VM -VM $vm -EnhancedSessionTransportType HvSocket
}

if (-not (Get-VMHost).EnableEnhancedSessionMode) {
    Say "turning on Enhanced Session Mode for this Hyper-V host"
    Set-VMHost -EnableEnhancedSessionMode $true
}

if (-not $NoStart) {
    Say "starting '$Name'"
    Start-VM -Name $Name
    Start-Process vmconnect.exe -ArgumentList "localhost", "`"$Name`""
    Say "VMConnect asks for a display size: that is the Enhanced Session, the desktop takes that size."
}
