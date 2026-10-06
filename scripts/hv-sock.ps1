<#
.SYNOPSIS
    Connect from the host to a Hyper-V socket service inside a VM (AF_HYPERV), send a line and
    print what comes back. Guests listening on a vsock-style port use the service GUID
    <port as 8 hex digits>-facb-11e6-bd58-64006a7986d3.
.EXAMPLE
    .\scripts\hv-sock.ps1 -Name Nocturne-G2 -Port 3389 -Send "hello"
#>
param(
    [string]$Name = "Nocturne-G2",
    [int]$Port = 3389,
    [string]$Send = "",
    [int]$TimeoutMs = 5000
)
$ErrorActionPreference = "Stop"
if (-not ("HvSock.HvEndPoint" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Net;
using System.Net.Sockets;
namespace HvSock {
public class HvEndPoint : EndPoint {
    public Guid VmId, ServiceId;
    public HvEndPoint(Guid vm, Guid svc) { VmId = vm; ServiceId = svc; }
    public override AddressFamily AddressFamily { get { return (AddressFamily)34; } }
    public override SocketAddress Serialize() {
        var sa = new SocketAddress((AddressFamily)34, 36);
        byte[] v = VmId.ToByteArray(), s = ServiceId.ToByteArray();
        for (int i = 0; i < 16; i++) { sa[4 + i] = v[i]; sa[20 + i] = s[i]; }
        return sa;
    }
    public override EndPoint Create(SocketAddress sa) { return this; }
}
}
"@
}
$vm = Get-VM -Name $Name
$svc = [Guid]("{0:x8}-facb-11e6-bd58-64006a7986d3" -f $Port)
$sock = New-Object System.Net.Sockets.Socket ([System.Net.Sockets.AddressFamily]34), ([System.Net.Sockets.SocketType]::Stream), ([System.Net.Sockets.ProtocolType]1)
$sock.ReceiveTimeout = $TimeoutMs
$sock.SendTimeout = $TimeoutMs
$t = Get-Date
$sock.Connect((New-Object HvSock.HvEndPoint $vm.Id, $svc))
"connected to $svc in {0:n0} ms" -f ((Get-Date) - $t).TotalMilliseconds
if ($Send) {
    [void]$sock.Send([Text.Encoding]::ASCII.GetBytes($Send))
    $buf = New-Object byte[] 4096
    try {
        $n = $sock.Receive($buf)
        "received $n bytes: " + [Text.Encoding]::ASCII.GetString($buf, 0, $n)
    } catch { "receive: $($_.Exception.InnerException.Message)" }
}
$sock.Close()
