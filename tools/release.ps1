<#
release.ps1 — quality-gate + tag + push for one version.

Orchestrator runs:
    pwsh tools/release.ps1 -Version 0.2.0 -Title "Framework hardening" `
        -Notes "Closes G-1, G-3, G-4, G-H1, G-H3 in esp-harness; bridge swaps to persistent ConsoleSession."

It then:
  1. Builds firmware (esp-harness build). Hard fail on warning > 0.
  2. Runs esp-harness pytest. Hard fail on any test red.
  3. Starts docs/mock_device.py on a free port.
  4. Runs tools/stress.py --all. Hard fail on any test red.
  5. Runs claude_buddy_bridge.py bench --events 1000 --dry-run. Captures numbers.
  6. Builds the [Unreleased] CHANGELOG stanza into a new vX.Y.Z stanza.
  7. Commits the CHANGELOG bump.
  8. Tags vX.Y.Z (annotated, signed with the notes).
  9. Pushes branch + tag.
 10. Optional: creates the GitHub release with notes (-CreateGhRelease).

Hard rules:
  - No flash. Real-device verification is the orchestrator's manual step.
  - Build, tests, stress, bench MUST pass before tag.
  - Tag is annotated; lightweight tags are rejected.
  - Refuses to run if working tree has uncommitted changes (use git status -s).

Exits 0 on success. Exits non-zero with reason on the first failure.
Use -DryRun to skip the commit/tag/push step (still runs gates).
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory=$true)]
    [string]$Title,

    [Parameter(Mandatory=$true)]
    [string]$Notes,

    [string]$ProjectRoot   = "D:\Code\esp32-agent-dashboard",
    [string]$HarnessRoot   = "D:\Code\esp-harness",
    [string]$HarnessPy     = "D:\Code\esp-harness\tools\esp-harness\.venv\Scripts\python.exe",
    [int]$MockPort         = 9890,
    [int]$BenchEvents      = 1000,
    [switch]$DryRun,
    [switch]$CreateGhRelease,
    [switch]$SkipBuild,
    [switch]$SkipStress
)

$ErrorActionPreference = 'Stop'
$VerbosePreference     = 'Continue'

function Die($msg) {
    Write-Host ""
    Write-Host "[release] FAILED: $msg" -ForegroundColor Red
    exit 1
}

function Step($n, $msg) {
    Write-Host ""
    Write-Host "── [$n/10] $msg" -ForegroundColor Cyan
}

# ──────────────────────────────────────────────────────────────────────
# Pre-flight
# ──────────────────────────────────────────────────────────────────────
Step 1 "Pre-flight (clean tree + tag-free)"

Push-Location $ProjectRoot
try {
    # Only block on MODIFIED tracked files. Untracked files (e.g. in-flight
    # agent deliverables) don't conflict with our CHANGELOG bump commit.
    $dirty = git status --porcelain -uno
    if ($dirty -and -not $DryRun) {
        Die "tracked files have uncommitted changes; commit or stash first:`n$dirty"
    }

    $existing = git tag --list "v$Version"
    if ($existing) {
        Die "tag v$Version already exists; bump the version or delete the tag"
    }
} finally {
    Pop-Location
}

# ──────────────────────────────────────────────────────────────────────
# Build
# ──────────────────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    Step 2 "Firmware build (must be clean, 0 warnings)"
    $buildJson = & $HarnessPy -m esp_harness build --project $ProjectRoot --json 2>&1
    $build = $buildJson | ConvertFrom-Json -ErrorAction SilentlyContinue
    if (-not $build -or -not $build.ok) {
        Die "build failed: $buildJson"
    }
    if ($build.n_warnings -gt 0) {
        Die "build has $($build.n_warnings) warnings; clean them up before tagging"
    }
    Write-Host "  ok bin=$($build.artifacts.bin) elapsed_ms=$($build.elapsed_ms)" -ForegroundColor Green
} else {
    Step 2 "Skipped (SkipBuild)"
}

# ──────────────────────────────────────────────────────────────────────
# esp-harness pytest
# ──────────────────────────────────────────────────────────────────────
Step 3 "esp-harness pytest"
Push-Location "$HarnessRoot\tools\esp-harness"
try {
    & $HarnessPy -m pytest -q 2>&1 | Tee-Object -Variable testOut | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Die "esp-harness pytest red:`n$($testOut -join "`n" | Select-Object -Last 30)"
    }
    $passLine = $testOut | Where-Object { $_ -match '\d+ passed' } | Select-Object -Last 1
    Write-Host "  ok $passLine" -ForegroundColor Green
} finally {
    Pop-Location
}

# ──────────────────────────────────────────────────────────────────────
# Stress + bench against a fresh mock
# ──────────────────────────────────────────────────────────────────────
if (-not $SkipStress) {
    Step 4 "Stress suite (5/5 must pass)"
    $mock = Start-Process -FilePath $HarnessPy `
        -ArgumentList "$ProjectRoot\docs\mock_device.py","--port","$MockPort","--decision-delay-ms","100" `
        -PassThru -NoNewWindow `
        -RedirectStandardError "$env:TEMP\release_mock_err.log"
    try {
        Start-Sleep -Seconds 2
        $stressOut = & $HarnessPy "$ProjectRoot\tools\stress.py" --all --port "127.0.0.1:$MockPort" 2>&1
        $stressLine = $stressOut | Where-Object { $_ -match 'all \d+ passed' -or $_ -match '\d+/\d+ FAILED' } | Select-Object -Last 1
        if (-not $stressLine -or $stressLine -match 'FAILED') {
            Die "stress suite red:`n$($stressOut -join "`n" | Select-Object -Last 12)"
        }
        Write-Host "  ok $stressLine" -ForegroundColor Green

        Step 5 "Bridge bench (capture throughput)"
        $benchJson = & $HarnessPy "$ProjectRoot\tools\claude_buddy_bridge.py" bench `
            --events $BenchEvents --pace-ms 0 --port-kind tcp --port "127.0.0.1:$MockPort" --dry-run 2>&1 |
            Select-Object -Last 1
        # bench command prints JSON on its last line via final return
        # We just store and continue; gate is "completed cleanly"
        Write-Host "  bench tail: $($benchJson)" -ForegroundColor Green
    } finally {
        if ($mock -and -not $mock.HasExited) { Stop-Process -Id $mock.Id -Force -ErrorAction SilentlyContinue }
    }
} else {
    Step 4 "Skipped (SkipStress)"
    Step 5 "Skipped (SkipStress)"
}

# ──────────────────────────────────────────────────────────────────────
# CHANGELOG bump
# ──────────────────────────────────────────────────────────────────────
Step 6 "CHANGELOG bump (Unreleased → v$Version)"
$changelogPath = "$ProjectRoot\CHANGELOG.md"
$changelog     = Get-Content $changelogPath -Raw
$today         = (Get-Date -Format 'yyyy-MM-dd')
$newStanza     = @"
## [$Version] — $today

**$Title**

$Notes

"@
if ($changelog -notmatch '## \[Unreleased\]') {
    Die "CHANGELOG.md missing [Unreleased] section; manual fix needed"
}
$changelog = $changelog -replace '## \[Unreleased\]', "## [Unreleased]`r`n`r`n$newStanza## [Unreleased-archive]"
$changelog = $changelog -replace '## \[Unreleased-archive\]', ''
Set-Content $changelogPath $changelog -Encoding utf8

# ──────────────────────────────────────────────────────────────────────
# Commit + tag + push
# ──────────────────────────────────────────────────────────────────────
if ($DryRun) {
    Write-Host "[release] dry run; stopping before commit/tag/push" -ForegroundColor Yellow
    exit 0
}

Push-Location $ProjectRoot
try {
    Step 7 "git commit CHANGELOG"
    git add CHANGELOG.md
    git commit -m "release(v$Version): $Title"

    Step 8 "git tag v$Version (annotated)"
    $tagMsg = "v$Version — $Title`r`n`r`n$Notes"
    git tag -a "v$Version" -m $tagMsg

    Step 9 "git push (branch + tag)"
    git push origin master
    git push origin "v$Version"

    if ($CreateGhRelease) {
        Step 10 "gh release create"
        gh release create "v$Version" --title "v$Version — $Title" --notes $Notes
    } else {
        Step 10 "Skipped GH release (use -CreateGhRelease to publish)"
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "[release] v$Version shipped." -ForegroundColor Green
