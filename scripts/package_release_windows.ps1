param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$OutDir = "release/windows",
    [string]$Commit = "",
    [string]$WorkflowRunUrl = ""
)

$ErrorActionPreference = "Stop"

$bundleCandidates = @(
    (Join-Path $BuildDir "mlrVST_artefacts/$Config/VST3/mlrVST.vst3"),
    (Join-Path $BuildDir "mlrVST_artefacts/VST3/mlrVST.vst3")
)

$bundle = $bundleCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bundle) {
    Write-Host "Could not find a Windows VST3 bundle in:"
    $bundleCandidates | ForEach-Object { Write-Host "  $_" }
    throw "Windows VST3 artifact not found."
}

if ([string]::IsNullOrWhiteSpace($Commit)) {
    try {
        $Commit = (git rev-parse HEAD).Trim()
    } catch {
        $Commit = "unknown"
    }
}

$shortSha = if ($Commit.Length -ge 7) { $Commit.Substring(0, 7) } else { $Commit }
$timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$packageName = "mlrVST-windows-x64-vst3-$timestamp-$shortSha"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$stageRoot = Join-Path $env:TEMP ("mlrvst-release-" + $timestamp + "-" + $shortSha)
$packageDir = Join-Path $stageRoot $packageName

if (Test-Path $stageRoot) {
    Remove-Item -Recurse -Force $stageRoot
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Copy-Item -Recurse -Path $bundle -Destination (Join-Path $packageDir "mlrVST.vst3")

foreach ($noticeFile in @("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")) {
    if (Test-Path $noticeFile) {
        Copy-Item -Path $noticeFile -Destination $packageDir
    }
}

$workflowField = if ([string]::IsNullOrWhiteSpace($WorkflowRunUrl)) { "n/a" } else { $WorkflowRunUrl }

@"
Product: mlrVST
Platform: Windows x64
Format: VST3
Commit: $Commit
Workflow run: $workflowField
Built at (UTC): $((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))
"@ | Set-Content -Path (Join-Path $packageDir "RELEASE_MANIFEST.txt")

$zipPath = Join-Path $OutDir ($packageName + ".zip")
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}
Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $zipPath -Force

Remove-Item -Recurse -Force $stageRoot

Write-Host "Created: $zipPath"
