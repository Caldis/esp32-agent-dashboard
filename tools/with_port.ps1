<#
.SYNOPSIS
  Run a command with exclusive ownership of the device serial port.

.DESCRIPTION
  The bridge daemon holds COM9 whenever it is running, and hook_dispatch.py
  AUTO-STARTS a new one whenever a tool-call hook can't reach one. That means
  any measurement session longer than a few seconds gets its port yanked
  mid-run by a hook firing in the background — during the v6.3 render work
  this killed three separate `?perf` sampling runs, each of which had to be
  restarted from scratch.

  dev_flash.ps1 already solves this for the flash step. This is the same
  dance factored out so it wraps ANYTHING:

      pwsh tools/with_port.ps1 { python -m esp_harness console --cmd "?stat" }
      pwsh tools/with_port.ps1 -TimeoutMin 30 { ./measure.ps1 }

  Sequence: write a future start_ts into the hook state file (every hook then
  sees itself as in cooldown and declines to spawn a bridge) -> kill any live
  bridge -> run the block -> restore the state file -> restart the bridge
  unless -NoBridge.

  The cooldown is removed and the bridge restarted even if the block throws,
  so a failed measurement never leaves the device unreachable.

.PARAMETER Command
  Script block to run while the port is owned exclusively.

.PARAMETER TimeoutMin
  How long the hook cooldown lasts (default 30). It is removed in the finally
  block regardless; this is only the backstop if the shell is killed outright.

.PARAMETER NoBridge
  Leave the bridge stopped afterwards (for chaining several wrapped runs).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [scriptblock] $Command,
    [int]  $TimeoutMin = 30,
    [switch] $NoBridge
)

$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent $PSScriptRoot
$state = Join-Path $env:TEMP 'claude_buddy_hook_state.json'

function Stop-Bridge {
    $procs = Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
             Where-Object { $_.CommandLine -match 'claude_buddy_bridge\.py' }
    foreach ($p in $procs) {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        Write-Host "with_port: stopped bridge pid $($p.ProcessId)"
    }
    if ($procs) { Start-Sleep -Milliseconds 1200 }   # let the OS release COM9
}

# 1. Put every hook into cooldown so none spawns a bridge behind our back.
$ts = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + ($TimeoutMin * 60)
"{`"start_ts`": $ts}" | Set-Content -Encoding utf8 $state

try {
    Stop-Bridge
    $env:CLAUDE_BUDDY_AUTOSTART = '0'   # belt and braces for children we spawn
    & $Command
}
finally {
    Remove-Item $state -ErrorAction SilentlyContinue
    Remove-Item Env:\CLAUDE_BUDDY_AUTOSTART -ErrorAction SilentlyContinue
    if (-not $NoBridge) {
        Start-Process -FilePath python `
            -ArgumentList 'tools/claude_buddy_bridge.py', 'serve', `
                          '--port-kind', 'serial', '--port', 'COM9' `
            -WorkingDirectory $repo -WindowStyle Hidden
        Write-Host "with_port: bridge restarted"
    }
}
