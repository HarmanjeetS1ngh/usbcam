# UsbCam tray: shows phone status (battery, temperature, stream) and lets you pick camera / resolution / fps from the PC.
# The choice is saved ON THE PHONE; the viewer and the OBS source use it when their size is left at 0 / default.
# Start with tray.bat (no console window). Needs adb on PATH and the UsbCam app installed on the phone.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$global:UcPort = 5002
$global:UcDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$global:UcCaps = $null
$global:UcStatus = $null
$global:UcLastErr = ''

# ---- virtual camera ("UsbCam Camera" for Meet / Zoom / Teams ...): the feeder program vcam\usbcam_vcam.exe, controlled on 127.0.0.1:5003 ----
$global:UcVcExe = Join-Path (Split-Path -Parent $global:UcDir) 'vcam\usbcam_vcam.exe'
$global:UcVcPort = 5003
$global:UcCfgPath = Join-Path $env:APPDATA 'UsbCam\tray.json'
$global:UcCfg = @{ vcam = $false; mirror = $false }
$global:UcVc = $null
try {
    if (Test-Path $global:UcCfgPath) {
        $j = Get-Content $global:UcCfgPath -Raw | ConvertFrom-Json
        $global:UcCfg.vcam = [bool]$j.vcam; $global:UcCfg.mirror = [bool]$j.mirror
    }
} catch { }

function global:UcCfgSave {
    try {
        New-Item -ItemType Directory -Force (Split-Path -Parent $global:UcCfgPath) | Out-Null
        $global:UcCfg | ConvertTo-Json | Set-Content $global:UcCfgPath
    } catch { }
}

function global:UcVcCtl([string]$json) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $iar = $c.BeginConnect('127.0.0.1', $global:UcVcPort, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(800)) { $c.Close(); return $null }
        $c.EndConnect($iar)
        $c.ReceiveTimeout = 2000
        $s = $c.GetStream()
        $w = New-Object IO.StreamWriter($s); $r = New-Object IO.StreamReader($s)
        $w.WriteLine($json); $w.Flush()
        $line = $r.ReadLine()
        $c.Close()
        if ($line) { return ($line | ConvertFrom-Json) }
    } catch { }
    return $null
}

function global:UcVcRunning { return [bool](Get-Process usbcam_vcam -ErrorAction SilentlyContinue) }

function global:UcVcStart([bool]$quiet) {
    if (UcVcRunning) { return }
    if (-not (Test-Path $global:UcVcExe)) {
        if (-not $quiet) { [void][Windows.Forms.MessageBox]::Show("Virtual camera program not found:`n$($global:UcVcExe)`nRun pc\build-vcam.ps1 first.") }
        return
    }
    if (-not (Test-Path 'HKLM:\SOFTWARE\Classes\CLSID\{6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}\InprocServer32')) {
        if (-not $quiet) { [void][Windows.Forms.MessageBox]::Show("The virtual camera is not installed yet.`nRun pc\vcam-install.ps1 once (one admin click).") }
        return
    }
    $m = '0'; if ($global:UcCfg.mirror) { $m = '1' }
    Start-Process -FilePath $global:UcVcExe -ArgumentList @('--mirror', $m) -WindowStyle Hidden -WorkingDirectory (Split-Path -Parent $global:UcVcExe)
    Start-Sleep -Milliseconds 1500
    if (-not (UcVcRunning) -and -not $quiet) {
        $tail = ''
        try { $tail = (Get-Content (Join-Path $env:TEMP 'usbcam-vcam.log') -Tail 3) -join "`n" } catch { }
        [void][Windows.Forms.MessageBox]::Show("The virtual camera could not start.`n$tail`nLog: $env:TEMP\usbcam-vcam.log")
    }
}

function global:UcVcStop {
    $null = UcVcCtl '{"cmd":"quit"}'
    for ($i = 0; $i -lt 15 -and (UcVcRunning); $i++) { Start-Sleep -Milliseconds 200 }
    Get-Process usbcam_vcam -ErrorAction SilentlyContinue | Stop-Process -Force
    $global:UcVc = $null
}

function global:UcVcLine {
    if ($global:UcVc -eq $null) { return '' }
    if ($global:UcVc.live) { return "Camera is live ($($global:UcVc.w)x$($global:UcVc.h))" }
    if ($global:UcVc.apps) { return 'An app is using it - waiting for the phone' }
    return "Ready - pick 'UsbCam Camera' in your app"
}

function global:UcCtl([string]$json) {
    # one short connection per command, so the viewer / OBS source can stay connected at the same time
    try {
        $c = New-Object Net.Sockets.TcpClient
        $iar = $c.BeginConnect('127.0.0.1', $global:UcPort, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(1500)) { $c.Close(); return $null }
        $c.EndConnect($iar)
        $c.ReceiveTimeout = 4000
        $s = $c.GetStream()
        $w = New-Object IO.StreamWriter($s); $r = New-Object IO.StreamReader($s)
        $w.WriteLine($json); $w.Flush()
        $line = $r.ReadLine()
        $c.Close()
        if ($line) { return ($line | ConvertFrom-Json) }
    } catch { $global:UcLastErr = $_.Exception.Message }
    return $null
}

function global:UcForward {
    & adb forward "tcp:$($global:UcPort)" "tcp:$($global:UcPort)" 2>$null | Out-Null
}

function global:UcWake {
    # start the phone app (needed once after a reboot / force-stop); the phone must be unlocked on Android 14+
    & adb shell am start -n com.harman.usbcam/.MainActivity 2>$null | Out-Null
}

function global:UcMakeIcon([string]$colorName) {
    $bmp = New-Object Drawing.Bitmap 16, 16
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([Drawing.Color]::Transparent)
    $g.FillEllipse((New-Object Drawing.SolidBrush ([Drawing.Color]::FromName($colorName))), 1, 1, 14, 14)
    $g.FillEllipse([Drawing.Brushes]::White, 5, 5, 6, 6)
    $g.Dispose()
    $ic = [Drawing.Icon]::FromHandle($bmp.GetHicon())
    return $ic
}

$global:UcIcons = @{
    off    = (UcMakeIcon 'Gray')
    idle   = (UcMakeIcon 'DodgerBlue')
    live   = (UcMakeIcon 'LimeGreen')
    warn   = (UcMakeIcon 'Orange')
    stop   = (UcMakeIcon 'Red')
}

$global:UcTray = New-Object Windows.Forms.NotifyIcon
$global:UcTray.Icon = $global:UcIcons['off']
$global:UcTray.Text = 'UsbCam: starting'
$global:UcTray.Visible = $true
$global:UcMenu = New-Object Windows.Forms.ContextMenuStrip
$global:UcTray.ContextMenuStrip = $global:UcMenu

function global:UcPick($cam, $w, $h, $fps) {
    $j = '{"cmd":"select","cam":"' + $cam + '","w":' + $w + ',"h":' + $h + ',"fps":' + $fps + ',"br":0,"apply":true}'
    $null = UcCtl $j
    UcRefresh
}

function global:UcAddModes($parent, $cam, $modes, $sel) {
    foreach ($m in $modes) {
        foreach ($f in $m.fps) {
            $t = "$($m.label)  $($m.w)x$($m.h) @ $f"
            if ($m.note) { $t += "  [$($m.note)]" }
            $it = New-Object Windows.Forms.ToolStripMenuItem $t
            if ($sel -and ($sel.cam -eq $cam) -and ($sel.w -eq $m.w) -and ($sel.h -eq $m.h) -and ($sel.fps -eq $f)) { $it.Checked = $true }
            $cc = $cam; $ww = $m.w; $hh = $m.h; $ff = $f
            $it.add_Click({ UcPick $cc $ww $hh $ff }.GetNewClosure())
            [void]$parent.DropDownItems.Add($it)
        }
    }
}

function global:UcBuildMenu {
    $global:UcMenu.Items.Clear()
    $st = $global:UcStatus
    $head = New-Object Windows.Forms.ToolStripMenuItem 'UsbCam'
    $head.Enabled = $false
    if ($st -eq $null) { $head.Text = 'Phone not reachable (plug in, unlock, enable USB debugging)' }
    else {
        $line = "$($st.state)"
        if ($st.actual) { $line += "  $($st.actual.w)x$($st.actual.h)@$($st.actual.fps)  $([math]::Round($st.encMbps,1)) Mbps" }
        $head.Text = $line
    }
    [void]$global:UcMenu.Items.Add($head)
    if ($st -ne $null) {
        $b = New-Object Windows.Forms.ToolStripMenuItem ("Battery $($st.batt)%" + $(if ($st.plugged) { ' (charging)' } else { '' }) + "  |  $([math]::Round($st.tempC,1)) C  |  thermal $($st.thermal)")
        $b.Enabled = $false
        [void]$global:UcMenu.Items.Add($b)
        if ($st.guard) {
            $gi = New-Object Windows.Forms.ToolStripMenuItem ("Guard: " + $st.guard)
            $gi.Enabled = $false
            [void]$global:UcMenu.Items.Add($gi)
        }
    }
    [void]$global:UcMenu.Items.Add((New-Object Windows.Forms.ToolStripSeparator))

    $camMenu = New-Object Windows.Forms.ToolStripMenuItem 'Camera and resolution (saved on phone)'
    if ($global:UcCaps -ne $null) {
        $sel = $null; if ($st -ne $null) { $sel = $st.sel }
        foreach ($c in $global:UcCaps.cameras) {
            if ($c.modes.Count -gt 0) {
                $ci = New-Object Windows.Forms.ToolStripMenuItem ("cam $($c.id) ($($c.facing), $([math]::Round($c.focal,1)) mm)")
                UcAddModes $ci $c.id $c.modes $sel
                [void]$camMenu.DropDownItems.Add($ci)
            }
            foreach ($p in $c.phys) {
                if ($p.modes.Count -gt 0) {
                    $pi = New-Object Windows.Forms.ToolStripMenuItem ("   lens $($p.id) ($([math]::Round($p.focal,1)) mm)")
                    UcAddModes $pi $p.id $p.modes $sel
                    [void]$camMenu.DropDownItems.Add($pi)
                }
            }
        }
    } else { $camMenu.Enabled = $false }
    [void]$global:UcMenu.Items.Add($camMenu)

    $stop = New-Object Windows.Forms.ToolStripMenuItem 'Stop stream'
    $stop.add_Click({ $null = UcCtl '{"cmd":"stop"}'; UcRefresh })
    [void]$global:UcMenu.Items.Add($stop)

    $gOn = $true; if ($st -ne $null -and $st.guardOn -eq $false) { $gOn = $false }
    $guard = New-Object Windows.Forms.ToolStripMenuItem 'Thermal / battery guard'
    $guard.Checked = $gOn
    $guard.add_Click({
        $new = 'true'; if ($global:UcStatus -ne $null -and $global:UcStatus.guardOn -ne $false) { $new = 'false' }
        $null = UcCtl ('{"cmd":"select","guard":' + $new + '}'); UcRefresh
    })
    [void]$global:UcMenu.Items.Add($guard)

    [void]$global:UcMenu.Items.Add((New-Object Windows.Forms.ToolStripSeparator))
    $vcOn = (UcVcRunning)
    $vc = New-Object Windows.Forms.ToolStripMenuItem ('Virtual camera: ' + $(if ($vcOn) { 'On' } else { 'Off' }))
    $vc.Checked = $vcOn
    $vc.add_Click({
        if (UcVcRunning) { $global:UcCfg.vcam = $false; UcCfgSave; UcVcStop }
        else { $global:UcCfg.vcam = $true; UcCfgSave; UcVcStart $false }
        UcRefresh
    })
    [void]$global:UcMenu.Items.Add($vc)
    if ($vcOn) {
        $vl = New-Object Windows.Forms.ToolStripMenuItem (UcVcLine)
        $vl.Enabled = $false
        [void]$global:UcMenu.Items.Add($vl)
    }
    $mir = New-Object Windows.Forms.ToolStripMenuItem 'Mirror picture'
    $mir.Checked = [bool]$global:UcCfg.mirror
    $mir.add_Click({
        $global:UcCfg.mirror = -not [bool]$global:UcCfg.mirror
        UcCfgSave
        $v = 'false'; if ($global:UcCfg.mirror) { $v = 'true' }
        if (UcVcRunning) { $null = UcVcCtl ('{"cmd":"mirror","on":' + $v + '}') }
        UcRefresh
    })
    [void]$global:UcMenu.Items.Add($mir)
    [void]$global:UcMenu.Items.Add((New-Object Windows.Forms.ToolStripSeparator))

    $viewer = New-Object Windows.Forms.ToolStripMenuItem 'Open viewer window'
    $viewer.add_Click({
        $exe = Join-Path (Split-Path -Parent $global:UcDir) 'viewer\usbcam_view.exe'
        if (Test-Path $exe) { Start-Process $exe -WorkingDirectory (Split-Path $exe) } else { [void][Windows.Forms.MessageBox]::Show("Viewer not found: $exe`nRun pc\build-viewer.ps1 first.") }
    })
    [void]$global:UcMenu.Items.Add($viewer)

    $wake = New-Object Windows.Forms.ToolStripMenuItem 'Open app on phone / reconnect'
    $wake.add_Click({ UcForward; UcWake; Start-Sleep -Seconds 2; UcRefresh })
    [void]$global:UcMenu.Items.Add($wake)

    [void]$global:UcMenu.Items.Add((New-Object Windows.Forms.ToolStripSeparator))
    $quit = New-Object Windows.Forms.ToolStripMenuItem 'Quit tray (also turns the virtual camera off)'
    $quit.add_Click({ UcVcStop; $global:UcTray.Visible = $false; [Windows.Forms.Application]::Exit() })
    [void]$global:UcMenu.Items.Add($quit)
}

function global:UcRefresh {
    $global:UcStatus = UcCtl '{"cmd":"status"}'
    if ($global:UcStatus -eq $null) {
        UcForward
        $global:UcStatus = UcCtl '{"cmd":"status"}'
    }
    if ($global:UcStatus -ne $null -and $global:UcCaps -eq $null) {
        $r = UcCtl '{"cmd":"caps"}'
        if ($r -ne $null) { $global:UcCaps = $r.caps }
    }
    $st = $global:UcStatus
    if ($st -eq $null) {
        $global:UcCaps = $null
        $global:UcTray.Icon = $global:UcIcons['off']
        $global:UcTray.Text = 'UsbCam: phone not reachable'
    } else {
        $icon = 'idle'
        if ($st.state -eq 'streaming') { $icon = 'live' }
        if ($st.guardStage -eq 1) { $icon = 'warn' }
        if ($st.guardStage -eq 2) { $icon = 'stop' }
        $global:UcTray.Icon = $global:UcIcons[$icon]
        $tip = "UsbCam: $($st.state) | $($st.batt)% $([math]::Round($st.tempC,0))C"
        if ($st.actual) { $tip = "UsbCam $($st.actual.w)x$($st.actual.h)@$($st.actual.fps) | $($st.batt)% $([math]::Round($st.tempC,0))C" }
        if ($tip.Length -gt 62) { $tip = $tip.Substring(0, 62) }
        $global:UcTray.Text = $tip
    }
    if (UcVcRunning) { $global:UcVc = UcVcCtl '{"cmd":"status"}' } else { $global:UcVc = $null }
    if (-not $global:UcMenu.Visible) { UcBuildMenu }   # do not rebuild under the user's cursor
}

UcForward
if ($global:UcCfg.vcam) { UcVcStart $true }
UcRefresh
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 4000
$timer.add_Tick({ UcRefresh })
$timer.Start()
[Windows.Forms.Application]::Run()
