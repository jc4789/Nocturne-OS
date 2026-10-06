<#
.SYNOPSIS
    Copy a Hyper-V VM's COM port named pipe to a log file (and the console) until the VM goes away.
.EXAMPLE
    .\scripts\hv-serial.ps1 -Pipe nocturne-g2-com1 -Log build\g2-serial.log
#>
param(
    [string]$Pipe = "nocturne-com1",
    [string]$Log = "build\serial.log",
    [int]$WaitSeconds = 60,
    [switch]$Quiet
)
$ErrorActionPreference = "Stop"
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ($true) {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", $Pipe, [System.IO.Pipes.PipeDirection]::InOut)
    try { $p.Connect(1000); break } catch { $p.Dispose() }
    if ((Get-Date) -gt $deadline) { throw "pipe \\.\pipe\$Pipe did not appear" }
}
$out = [System.IO.File]::Open($Log, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
$buf = New-Object byte[] 4096
try {
    while ($true) {
        $n = $p.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $out.Write($buf, 0, $n)
        $out.Flush()
        if (-not $Quiet) { [Console]::Out.Write([System.Text.Encoding]::UTF8.GetString($buf, 0, $n)) }
    }
}
finally {
    $out.Dispose()
    $p.Dispose()
}
