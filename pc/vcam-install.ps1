# Installs the "UsbCam Camera" virtual camera component (needs Windows 11).
# What it does, in plain words:
#   1. copies one small file (usbcam_vcam_src.dll) to C:\Program Files\UsbCam\vcam (Windows' camera service must be able to read it)
#   2. registers it with Windows (one registry entry)
# It asks for administrator rights (one "Yes" click). Nothing else is changed. Undo with pc\vcam-uninstall.ps1.
# Log: %TEMP%\usbcam-vcam-install.log
param([string]$Source = '', [switch]$Elevated)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root 'vcam' }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ([Environment]::OSVersion.Version.Build -lt 22000) {
    Write-Host "The virtual camera needs Windows 11 (build 22000 or newer). You have build $([Environment]::OSVersion.Version.Build)." -ForegroundColor Red
    Write-Host "Send Claude the output of: winver"
    exit 1
}
if (-not (Test-Path (Join-Path $Source 'usbcam_vcam_src.dll'))) {
    Write-Host "Not found: $Source\usbcam_vcam_src.dll. Run pc\build-vcam.ps1 first." -ForegroundColor Red
    exit 1
}
if (-not $isAdmin) {
    Write-Host "Asking for administrator rights (click Yes)..."
    $args2 = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Source `"$Source`" -Elevated"
    Start-Process powershell -Verb RunAs -ArgumentList $args2 -Wait
    exit 0
}

$log = Join-Path $env:TEMP 'usbcam-vcam-install.log'
Start-Transcript -Path $log -Force | Out-Null
try {
    Get-Process usbcam_vcam -ErrorAction SilentlyContinue | Stop-Process -Force
    $dest = Join-Path $env:ProgramFiles 'UsbCam\vcam'
    New-Item -ItemType Directory -Force $dest | Out-Null
    # an older copy may still be loaded by Windows' camera service: stop it so the file can be replaced (it restarts by itself)
    Stop-Service -Name FrameServer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Copy-Item (Join-Path $Source 'usbcam_vcam_src.dll') $dest -Force
    $dll = Join-Path $dest 'usbcam_vcam_src.dll'
    $rp = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s `"$dll`"" -Wait -PassThru
    if ($rp.ExitCode -ne 0) { throw "regsvr32 failed with code $($rp.ExitCode)" }
    $key = 'HKLM:\SOFTWARE\Classes\CLSID\{6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}\InprocServer32'
    if (-not (Test-Path $key)) { throw 'registration did not show up in the registry' }
    Write-Host ""
    Write-Host "Installed: $dll" -ForegroundColor Green
    Write-Host "Next: start pc\tray.bat, right-click the tray icon > 'Virtual camera' > On."
    Write-Host "Then pick 'UsbCam Camera' in Meet / Zoom / Teams / Discord / Windows Camera."
} catch {
    Write-Host "Install failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Close apps that use the camera (Teams, Zoom, Camera app) and run this again. Log: $log"
}
Stop-Transcript | Out-Null
if ($Elevated) { Read-Host "Press Enter to close" | Out-Null }
