# run.ps1 - laptop side: deploy the build to the Pi and run the whole demo.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1            deploy + start (enforcing)
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Off       deploy + start, taskmgr watch-only
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Action stop
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Action status
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Action killtm
#
# "start": check the build -> stop the old run -> copy programs, dashboard and
# demo.sh -> open 3 windows: SYSTEM (starts all actors), PANEL (keys 1/2/s/q),
# DASHBOARD. Each window asks for the Pi's root password once (su).
# Other actions run the matching /tmp/demo.sh command as root.
# Build in Momentics first.

param(
    [ValidateSet("start", "stop", "status", "killtm")]
    [string]$Action = "start",
    [switch]$Off
)

$ErrorActionPreference = "Stop"
$Pi  = "qnxuser@10.0.0.1"
$Mac = "hmac-sha2-256"
Set-Location (Split-Path -Parent $PSScriptRoot)        # project root (folder above scripts\)

function Invoke-PiRoot([string]$Shortcut) {
    # run /tmp/demo-<Shortcut> as root in this window (root password prompt)
    ssh -t -m $Mac $Pi su -c /tmp/demo-$Shortcut
}

function Open-PiWindow([string]$Title, [string]$Shortcut) {
    $cmd = "`$host.UI.RawUI.WindowTitle='$Title'; ssh -t -m $Mac $Pi su -c /tmp/demo-$Shortcut"
    Start-Process powershell -ArgumentList "-NoExit", "-Command", $cmd
}

if ($Action -ne "start") {
    Invoke-PiRoot $Action
    exit
}

# 1. every program must be built
$bins  = "build\taskmgr", "build\abs", "build\airbag", "build\decoder", "build\nav", "build\ui", "build\panel"
$files = $bins + "tools\dashboard.py", "scripts\demo.sh"
foreach ($f in $files) {
    if (-not (Test-Path $f)) { Write-Host "MISSING: $f  (build in Momentics first)" -ForegroundColor Red; exit 1 }
}

# 2. stop the previous run, so running programs don't block the copy
Write-Host "Stopping previous run on the Pi (root password if asked)..." -ForegroundColor Cyan
ssh -m $Mac $Pi "test -x /tmp/demo-stop"
if ($LASTEXITCODE -eq 0) { Invoke-PiRoot "stop" }

# 3. copy programs, demo.sh and the dashboard (as coremap.py) to /tmp
Write-Host "Copying programs..." -ForegroundColor Cyan
scp -o "MACs=$Mac" $bins scripts\demo.sh "${Pi}:/tmp/"
if ($LASTEXITCODE -ne 0) { Write-Host "scp failed" -ForegroundColor Red; exit 1 }
scp -o "MACs=$Mac" tools\dashboard.py "${Pi}:/tmp/coremap.py"
if ($LASTEXITCODE -ne 0) { Write-Host "scp of dashboard failed" -ForegroundColor Red; exit 1 }

# 4. on the Pi: strip Windows line endings, make executable, create /tmp/demo-* shortcuts
ssh -m $Mac $Pi "tr -d '\r' < /tmp/demo.sh > /tmp/demo.tmp && mv /tmp/demo.tmp /tmp/demo.sh && chmod +x /tmp/demo.sh /tmp/taskmgr /tmp/abs /tmp/airbag /tmp/decoder /tmp/nav /tmp/ui /tmp/panel && sh /tmp/demo.sh install"
if ($LASTEXITCODE -ne 0) { Write-Host "setup on the Pi failed" -ForegroundColor Red; exit 1 }

# 5. open the three demo windows
$start = if ($Off) { "startoff" } else { "start" }
Open-PiWindow "SYSTEM"    $start
Open-PiWindow "PANEL"     "panel"
Open-PiWindow "DASHBOARD" "dash"
Write-Host "Opened SYSTEM, PANEL, DASHBOARD. Type the root password in SYSTEM first." -ForegroundColor Green
Write-Host "PANEL: s = storm on/off, q = quit.   Stop all: scripts\run.ps1 -Action stop" -ForegroundColor Green
