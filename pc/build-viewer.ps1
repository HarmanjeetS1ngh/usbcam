# Pushes the project, waits for the Windows build of the PC viewer on GitHub, downloads it to <project>\viewer
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
git add -A
git commit -q -m "viewer update" 2>$null
git push
if ($LASTEXITCODE -ne 0) { throw "git push failed" }
$sha = (git rev-parse HEAD).Trim()
$id = ""
for ($i = 0; $i -lt 12 -and -not $id; $i++) {
    Start-Sleep 5
    $id = (gh run list --workflow build-viewer.yml --commit $sha --limit 1 --json databaseId -q '.[0].databaseId' 2>$null | Out-String).Trim()
}
if (-not $id) { throw "No viewer build found. Open the repo's Actions tab on GitHub." }
Write-Host "Building on GitHub (about 3-6 minutes)..."
gh run watch $id --exit-status
if ($LASTEXITCODE -ne 0) {
    gh run view $id --log-failed
    throw "Viewer build failed - send the log above to Claude"
}
Remove-Item "$root\viewer" -Recurse -Force -ErrorAction SilentlyContinue
gh run download $id -n usbcam-viewer -D "$root\viewer"
Write-Host "Done: $root\viewer\usbcam_view.exe" -ForegroundColor Green
