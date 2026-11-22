# Pushes the project, waits for the OBS plugin build on GitHub, downloads it and installs it for your user.
# Close OBS first (it locks the plugin files). Run: powershell -ExecutionPolicy Bypass -File pc\build-obs.ps1
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw "Close OBS first, then run this again." }
git add -A
git commit -q -m "obs plugin update" 2>$null
git push
if ($LASTEXITCODE -ne 0) { throw "git push failed" }
$sha = (git rev-parse HEAD).Trim()
$id = ""
for ($i = 0; $i -lt 12 -and -not $id; $i++) {
    Start-Sleep 5
    $id = (gh run list --workflow build-obs.yml --commit $sha --limit 1 --json databaseId -q '.[0].databaseId' 2>$null | Out-String).Trim()
}
if (-not $id) { throw "No OBS build found. Open the repo's Actions tab on GitHub." }
Write-Host "Building on GitHub (about 3-6 minutes)..."
gh run watch $id --exit-status
if ($LASTEXITCODE -ne 0) {
    gh run view $id --log-failed
    throw "OBS plugin build failed - send the log above to Claude"
}
Remove-Item "$root\obs-plugin" -Recurse -Force -ErrorAction SilentlyContinue
gh run download $id -n usbcam-obs-plugin -D "$root\obs-plugin"
# OBS 33 only scans C:\ProgramData\obs-studio\plugins (not %APPDATA%); legacy layout = <name>\bin\64bit + <name>\data
$dest = Join-Path $env:ProgramData "obs-studio\plugins\usbcam-obs"
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$root\obs-plugin\usbcam-obs\*" $dest -Recurse -Force
New-Item -ItemType Directory -Force "$dest\data" | Out-Null
Remove-Item (Join-Path $env:APPDATA "obs-studio\plugins\usbcam-obs") -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Installed to $dest" -ForegroundColor Green
Write-Host "Now open OBS > Sources > + > 'UsbCam (USB phone camera)'. Phone plugged in and unlocked."
