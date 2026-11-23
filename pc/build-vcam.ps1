# Pushes the project, waits for the Windows build of the virtual camera on GitHub, downloads it to <project>\vcam,
# then installs the camera component (one admin "Yes" click). Use -NoInstall to only download.
param([switch]$NoInstall)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
git add -A
git commit -q -m "virtual camera update" 2>$null
git push
if ($LASTEXITCODE -ne 0) { throw "git push failed" }
$sha = (git rev-parse HEAD).Trim()
$id = ""
for ($i = 0; $i -lt 12 -and -not $id; $i++) {
    Start-Sleep 5
    $id = (gh run list --workflow build-vcam.yml --commit $sha --limit 1 --json databaseId -q '.[0].databaseId' 2>$null | Out-String).Trim()
}
if (-not $id) { throw "No virtual camera build found. Open the repo's Actions tab on GitHub." }
Write-Host "Building on GitHub (about 3-6 minutes)..."
gh run watch $id --exit-status
if ($LASTEXITCODE -ne 0) {
    gh run view $id --log-failed
    throw "Virtual camera build failed - send the log above to Claude"
}
Get-Process usbcam_vcam -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item "$root\vcam" -Recurse -Force -ErrorAction SilentlyContinue
gh run download $id -n usbcam-vcam -D "$root\vcam"
Write-Host "Downloaded to $root\vcam" -ForegroundColor Green
if (-not $NoInstall) { & "$PSScriptRoot\vcam-install.ps1" -Source "$root\vcam" }
