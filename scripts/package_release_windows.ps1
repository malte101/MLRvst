param(
    [string]$BuildDir = "Build",
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

$runtimeNames = @(
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)

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

$bundleRuntimeDir = Join-Path $packageDir "mlrVST.vst3/Contents/x86_64-win"
$sourceRuntimeDirs = @(
    (Join-Path $bundle "Contents/x86_64-win"),
    (Split-Path $bundle -Parent),
    $BuildDir
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

$bundledRuntimeNames = New-Object System.Collections.Generic.List[string]

foreach ($runtimeName in $runtimeNames) {
    $alreadyBundled = Join-Path $bundleRuntimeDir $runtimeName
    if (Test-Path $alreadyBundled) {
        $bundledRuntimeNames.Add($runtimeName)
        continue
    }

    $runtimeSource = $null
    foreach ($runtimeDir in $sourceRuntimeDirs) {
        $candidate = Join-Path $runtimeDir $runtimeName
        if (Test-Path $candidate) {
            $runtimeSource = $candidate
            break
        }
    }

    if ($runtimeSource) {
        Copy-Item -Path $runtimeSource -Destination $alreadyBundled
        $bundledRuntimeNames.Add($runtimeName)
    }
}

foreach ($noticeFile in @("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md")) {
    if (Test-Path $noticeFile) {
        Copy-Item -Path $noticeFile -Destination $packageDir
    }
}

$licenseDir = Join-Path $packageDir "LICENSES"
New-Item -ItemType Directory -Path $licenseDir -Force | Out-Null

foreach ($licenseSpec in @(
    @{ Source = "third_party/signalsmith-stretch/LICENSE.txt"; Target = "signalsmith-stretch-LICENSE.txt" },
    @{ Source = "third_party/signalsmith-linear/LICENSE.txt"; Target = "signalsmith-linear-LICENSE.txt" }
)) {
    if (Test-Path $licenseSpec.Source) {
        Copy-Item -Path $licenseSpec.Source -Destination (Join-Path $licenseDir $licenseSpec.Target)
    }
}

$workflowField = if ([string]::IsNullOrWhiteSpace($WorkflowRunUrl)) { "n/a" } else { $WorkflowRunUrl }
$runtimeField = if ($bundledRuntimeNames.Count -gt 0) { ($bundledRuntimeNames -join ", ") } else { "none" }

@"
Product: mlrVST
Platform: Windows x64
Format: VST3
Commit: $Commit
Workflow run: $workflowField
Bundled runtimes: $runtimeField
Built at (UTC): $((Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ"))
"@ | Set-Content -Path (Join-Path $packageDir "RELEASE_MANIFEST.txt")

$zipPath = Join-Path $OutDir ($packageName + ".zip")
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}
Compress-Archive -Path (Join-Path $stageRoot "*") -DestinationPath $zipPath -Force

Remove-Item -Recurse -Force $stageRoot

Write-Host "Created: $zipPath"
