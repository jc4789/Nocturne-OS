<#
.SYNOPSIS
    Development loop for Hyper-V: (re)boot a VM on the freshly built ISO, record its COM1 log and
    take a screenshot after a delay. Needs an elevated shell.
.EXAMPLE
    .\scripts\hv-boot.ps1 -Name Nocturne-G2 -Seconds 20
#>
param(
    [string]$Name = "Nocturne-G2",
    [int]$Seconds = 20,
    [string]$Out = "build\hv",
    [switch]$Build
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if ($Build) {
    $b = & powershell -ExecutionPolicy Bypass -File "$root\build.ps1" 2>&1
    $bad = $b | Select-String -Pattern "error|warning:"
    if ($bad -or $LASTEXITCODE) { $bad | Select-Object -First 30; throw "build failed" }
}
New-Item -ItemType Directory -Force $Out | Out-Null

$vm = Get-VM -Name $Name
if ($vm.State -ne 'Off') { Stop-VM -VM $vm -TurnOff -Force }
$dvd = Get-VMDvdDrive -VM $vm | Select-Object -First 1
if ($dvd -and $dvd.Path) { Copy-Item build\nocturne.iso $dvd.Path -Force }
# a VM that boots from a disk image instead: refresh its *-boot.vhdx
Get-VMHardDiskDrive -VM $vm | Where-Object { $_.Path -like '*-boot.vhdx' } |
    ForEach-Object { Copy-Item build\nocturne.vhdx $_.Path -Force }

$pipe = ((Get-VMComPort -VM $vm -Number 1).Path -replace '^\\\\\.\\pipe\\', '')
$log = Join-Path (Resolve-Path $Out) "$Name-serial.log"
Start-VM -VM $vm
# Start-Process joins arguments with spaces, so paths need their own quotes.
$logger = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\hv-serial.ps1`"",
    '-Pipe', $pipe, '-Log', "`"$log`"", '-Quiet')
Start-Sleep -Seconds $Seconds
& "$PSScriptRoot\hv-shot.ps1" -Name $Name -Out (Join-Path $Out "$Name.png")
Write-Host "---- $log ----"
Get-Content $log -ErrorAction SilentlyContinue
