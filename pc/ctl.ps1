# Send one JSON command to the phone's control channel and print the reply.
# Usage:  .\ctl.ps1 '{"cmd":"caps"}'     (use single quotes around the JSON)
param([string]$Json = '{"cmd":"hello"}', [int]$Port = 5002)
adb forward "tcp:$Port" "tcp:$Port" | Out-Null
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port)
$c.ReceiveTimeout = 8000
$s = $c.GetStream()
$w = New-Object IO.StreamWriter($s); $r = New-Object IO.StreamReader($s)
$w.WriteLine($Json); $w.Flush()
$r.ReadLine()
$c.Close()
