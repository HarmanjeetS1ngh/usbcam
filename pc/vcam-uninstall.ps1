# Removes the "UsbCam Camera" virtual camera completely.
# What it does, in plain words: stops the virtual camera program, removes the registry entry, deletes
# C:\Program Files\UsbCam\vcam and the tray's saved On/Mirror setting. The phone app, viewer and OBS plugin are not touched.
# It asks for administrator rights (one "Yes" click). Log: %TEMP%\usbcam-vcam-install.log
param([switch]$Elevated)
$ErrorActionPreference = 'Continue'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Asking for administrator rights (click Yes)..."
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Elevated" -Wait
    exit 0
}
$log = Join-Path $env:TEMP 'usbcam-vcam-install.log'
Start-Transcript -Path $log -Force | Out-Null
Get-Process usbcam_vcam -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Service -Name FrameServer -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
$dest = Join-Path $env:ProgramFiles 'UsbCam\vcam'
$dll = Join-Path $dest 'usbcam_vcam_src.dll'
if (Test-Path $dll) { & "$env:SystemRoot\System32\regsvr32.exe" /u /s "$dll" }
# in case the DLL was already gone: remove the registry entry directly
Remove-Item 'HKLM:\SOFTWARE\Classes\CLSID\{6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
$parent = Split-Path -Parent $dest
if ((Test-Path $parent) -and -not (Get-ChildItem $parent -Force -ErrorAction SilentlyContinue)) { Remove-Item $parent -Force -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $env:APPDATA 'UsbCam\tray.json') -Force -ErrorAction SilentlyContinue
Start-Service -Name FrameServer -ErrorAction SilentlyContinue
$left = @(Get-PnpDevice -FriendlyName 'UsbCam Camera' -ErrorAction SilentlyContinue)
$regLeft = Test-Path 'HKLM:\SOFTWARE\Classes\CLSID\{6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}'
if ((Test-Path $dest) -or $regLeft) { Write-Host "Some parts could not be removed (a program still uses the file). Close camera apps, restart Windows, run again." -ForegroundColor Yellow }
elseif ($left.Count -gt 0) { Write-Host "Removed. Windows may still list the camera until the next restart." -ForegroundColor Yellow }
else { Write-Host "UsbCam Camera removed completely." -ForegroundColor Green }
Stop-Transcript | Out-Null
if ($Elevated) { Read-Host "Press Enter to close" | Out-Null }
