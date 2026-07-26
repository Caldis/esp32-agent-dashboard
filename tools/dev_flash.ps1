# dev_flash.ps1 - one-shot build + flash + verify with bridge handling.
#
# Automates the COM9 contention dance documented in CLAUDE.md ("Flashing"):
#   1. put hooks in cooldown (a future start_ts) so no hook can auto-start
#      a bridge that would grab COM9 mid-flash;
#   2. kill any running bridge;
#   3. run `esp-harness cycle` (build -> flash -> verify) with PYTHONUTF8=1
#      so subprocess decoding never trips over zh-CN GBK;
#   4. ALWAYS remove the cooldown file (finally-block). The bridge is
#      restarted ONLY on success - after a failed flash the device may be
#      re-enumerating and a restarted bridge would seize COM9 the moment
#      it reappears, fighting the recovery reflash (learned 2026-07-26).
#      On failure the next tool-call hook auto-starts the bridge anyway.
#
# Usage:  pwsh tools/dev_flash.ps1 [-NoBridge]
param([switch]$NoBridge)

$repo  = Split-Path $PSScriptRoot -Parent
$state = Join-Path $env:TEMP 'claude_buddy_hook_state.json'

# 1. hook cooldown (1h ceiling; removed in finally long before that)
$ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 3600
"{""start_ts"": $ts}" | Set-Content -Path $state

# 2. kill bridge - match python processes only, so this never touches the
#    calling shell (whose command line also mentions the bridge script)
Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
    Where-Object { $_.CommandLine -match 'claude_buddy_bridge\.py' } |
    ForEach-Object {
        Write-Host "stopping bridge PID $($_.ProcessId)"
        try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch {}
    }
Start-Sleep -Milliseconds 1500  # let Windows release the COM handle

# 3. build + flash + verify
$env:PYTHONUTF8 = '1'
$code = 1
try {
    Push-Location $repo
    python -m esp_harness cycle
    $code = $LASTEXITCODE
    Pop-Location
}
finally {
    # 4. re-enable hook autostart no matter what happened above
    Remove-Item $state -Force -ErrorAction SilentlyContinue
    if ($code -eq 0 -and -not $NoBridge) {
        Start-Process -FilePath python `
            -ArgumentList 'tools/claude_buddy_bridge.py','serve','--port-kind','serial','--port','COM9' `
            -WorkingDirectory $repo -WindowStyle Hidden
        Write-Host "bridge restarted"
    } elseif ($code -ne 0) {
        Write-Host "flash FAILED (exit $code) - bridge NOT restarted so COM9 stays free for recovery."
        Write-Host "Recover with: python -m esp_harness flash --project $repo   (then rerun this script or let a hook restart the bridge)"
    }
}
exit $code
