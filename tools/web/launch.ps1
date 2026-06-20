# launch.ps1 — one-command start for the web dev panel (real device + mirror).
#
#   ./launch.ps1            # drive COM9, observe mode, web on :8090
#   ./launch.ps1 COM5       # different serial port
#   ./launch.ps1 -Mock      # no hardware: mock TCP device
#
# Then open http://127.0.0.1:8090/
param(
    [string]$Serial = "COM9",
    [switch]$Mock
)
$serve = Join-Path $PSScriptRoot "serve.py"
if ($Mock) {
    Write-Host "[launch] mock mode — web on http://127.0.0.1:8090/"
    & python $serve --spawn-bridge
} else {
    Write-Host "[launch] real device on $Serial — web on http://127.0.0.1:8090/"
    & python $serve --spawn-bridge --serial $Serial --auto off
}
