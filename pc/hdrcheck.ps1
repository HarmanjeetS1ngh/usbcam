# Reads framed video (start with hdr=true) from port 5000 for a few seconds and prints frame stats.
# Usage:  .\hdrcheck.ps1 [-Seconds 3]
param([int]$Seconds = 3, [int]$Port = 5000)
adb forward "tcp:$Port" "tcp:$Port" | Out-Null
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port)
$s = $c.GetStream()
function ReadN([int]$n) {
    $b = New-Object byte[] $n; $o = 0
    while ($o -lt $n) { $k = $s.Read($b, $o, $n - $o); if ($k -le 0) { throw 'connection closed' }; $o += $k }
    , $b
}
$end = (Get-Date).AddSeconds($Seconds)
$frames = 0; $keys = 0; $cfgs = 0; $bytes = 0L; $first = $null; $last = $null; $maxGap = 0L
while ((Get-Date) -lt $end) {
    $h = ReadN 13
    $len = ([int]$h[0] -shl 24) -bor ([int]$h[1] -shl 16) -bor ([int]$h[2] -shl 8) -bor [int]$h[3]
    [long]$pts = 0
    for ($i = 4; $i -lt 12; $i++) { $pts = ($pts -shl 8) -bor [long]$h[$i] }
    $flags = [int]$h[12]
    $null = ReadN $len
    $bytes += $len
    if ($flags -band 2) { $cfgs++; continue }
    $frames++
    if ($flags -band 1) { $keys++ }
    if ($null -ne $last -and ($pts - $last) -gt $maxGap) { $maxGap = $pts - $last }
    if ($null -eq $first) { $first = $pts }
    $last = $pts
}
$c.Close()
$span = ($last - $first) / 1e6
"frames=$frames keyframes=$keys config=$cfgs avgFps=$([math]::Round(($frames - 1) / $span, 1)) maxGapMs=$([math]::Round($maxGap / 1000, 1)) Mbps=$([math]::Round($bytes * 8 / $Seconds / 1e6, 1))"
