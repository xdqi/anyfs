# Windows counterpart of test-device.sh: attach a VHDX as a READ-ONLY disk for
# device-I/O tests, and detach it again. Needs an elevated shell (CI runner).
#
#   test-device.ps1 attach <parts.vhdx> <parts.img> <state-dir>
#     Mount-DiskImage -Access ReadOnly -NoDriveLetter. The disk is taken only
#     from Get-DiskImage for this VHDX and verified against the RAW image it
#     was converted from (not the VHDX container): same size as parts.img,
#     read-only, and the first MiB read from \\.\PhysicalDriveN equals
#     parts.img's first MiB. Partition paths (\\.\HarddiskNPartitionK) are
#     recorded only if they actually open.
#   test-device.ps1 detach <state-dir>
#     Dismount-DiskImage, check it is detached and that the VHDX's SHA-256
#     did not change. device.env marks an attached device; device.json stays
#     as a record.
#
# Writes <state-dir>/device.env (bash-sourceable) and device.json, with the
# keys test-device.sh documents.
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('attach', 'detach')][string]$Action,
    [Parameter(Position = 1)][string]$A1,
    [Parameter(Position = 2)][string]$A2,
    [Parameter(Position = 3)][string]$A3
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RawDev {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa,
                                             uint disposition, uint flags, IntPtr template);
    // GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, OPEN_EXISTING
    public static byte[] Head(string path, int n) {
        using (var h = CreateFileW(path, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new IOException(path + ": CreateFile error " + Marshal.GetLastWin32Error());
            using (var fs = new FileStream(h, FileAccess.Read)) {
                var buf = new byte[n]; int got = 0;
                while (got < n) { int r = fs.Read(buf, got, n - got); if (r <= 0) break; got += r; }
                if (got != n) throw new IOException(path + ": short read " + got);
                return buf;
            }
        }
    }
}
'@

function Sha([string]$p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLower() }
function Q([string]$s) { "'" + $s.Replace("'", "'\''") + "'" }   # bash single-quoting

function Write-State($dir, $o) {
    $o | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $dir 'device.json')
    @(
        "ANYFS_TEST_KIND=vhdx"
        "ANYFS_TEST_DEVICE=$(Q $o.device)"
        "ANYFS_TEST_RAW_DEVICE="
        "ANYFS_TEST_PART_DEVICES=$(Q ($o.partitions -join ' '))"
        "ANYFS_TEST_FIXTURE=$(Q $o.fixture)"
        "ANYFS_TEST_FIXTURE_SHA256=$(Q $o.fixtureSha256)"
    ) | Set-Content -Encoding ascii (Join-Path $dir 'device.env')
}

if ($Action -eq 'attach') {
    if (-not $A3) { throw 'usage: test-device.ps1 attach <parts.vhdx> <parts.img> <state-dir>' }
    $vhdx = (Resolve-Path -LiteralPath $A1).Path
    $raw = (Resolve-Path -LiteralPath $A2).Path
    $state = $A3
    New-Item -ItemType Directory -Force -Path $state | Out-Null
    if (Test-Path (Join-Path $state 'device.env')) { throw "$state already describes an attached device" }
    $sum = Sha $vhdx
    # Recorded before mounting, so detach can clean up whatever happens next.
    Write-State $state ([ordered]@{ device = ''; rawDevice = $null; partitions = @(); fixture = $vhdx; fixtureSha256 = $sum; readOnly = $true })

    Mount-DiskImage -ImagePath $vhdx -StorageType VHDX -Access ReadOnly -NoDriveLetter | Out-Null
    $di = Get-DiskImage -ImagePath $vhdx
    if (-not $di.Attached -or $null -eq $di.Number) { throw "$vhdx did not attach" }
    $disk = Get-Disk -Number $di.Number
    $dev = "\\.\PhysicalDrive$($disk.Number)"
    $rawSize = (Get-Item -LiteralPath $raw).Length
    if ($disk.Size -ne $rawSize) { throw "$dev is $($disk.Size) bytes, $raw is $rawSize" }
    if (-not $disk.IsReadOnly) { throw "$dev is not read-only" }
    $mib = 1048576
    $want = [byte[]]::new($mib)
    $fs = [IO.File]::OpenRead($raw); try { [void]$fs.Read($want, 0, $mib) } finally { $fs.Dispose() }
    $got = [RawDev]::Head($dev, $mib)
    if ([Convert]::ToBase64String($got) -ne [Convert]::ToBase64String($want)) { throw "first MiB of $dev differs from $raw" }

    $parts = @()
    foreach ($p in (Get-Partition -DiskNumber $disk.Number | Sort-Object PartitionNumber)) {
        $path = "\\.\Harddisk$($disk.Number)Partition$($p.PartitionNumber)"
        try { [void][RawDev]::Head($path, 4096); $parts += $path }
        catch { Write-Host "test-device: $path does not open: $($_.Exception.Message)"; $parts += '' }
    }
    $o = [ordered]@{ device = $dev; rawDevice = $null; partitions = $parts; fixture = $vhdx; fixtureSha256 = $sum; readOnly = $true }
    Write-State $state $o
    Write-Host "test-device: $vhdx attached read-only as $dev (disk $($disk.Number), $($disk.FriendlyName)); partitions: $($parts -join ' ')"
    Get-Disk -Number $disk.Number | Format-List Number, FriendlyName, Size, IsReadOnly, PartitionStyle, Location
}
else {
    $state = $A1
    $json = Join-Path $state 'device.json'
    if (-not (Test-Path (Join-Path $state 'device.env'))) { Write-Host "test-device: nothing attached in $state"; exit 0 }
    $o = Get-Content -Raw $json | ConvertFrom-Json
    Dismount-DiskImage -ImagePath $o.fixture | Out-Null
    if ((Get-DiskImage -ImagePath $o.fixture).Attached) { throw "$($o.fixture) is still attached" }
    $now = Sha $o.fixture
    if ($now -ne $o.fixtureSha256) { throw "$($o.fixture) changed while attached ($($o.fixtureSha256) -> $now)" }
    Remove-Item (Join-Path $state 'device.env')   # device.json stays, as a record
    Write-Host "test-device: detached $($o.device); $($o.fixture) unchanged (sha256 $now)"
}
