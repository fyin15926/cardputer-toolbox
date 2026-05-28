param(
  [ValidateSet("Queue", "Paste", "Send")]
  [string]$DispatcherMode = "Queue",
  [string]$WindowTitle = "Codex",
  [int]$IntervalSeconds = 2
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$inbox = Join-Path $root "codex-inbox"
$bridgeScript = Join-Path $PSScriptRoot "chat_inbox_bridge.ps1"
$dispatcherScript = Join-Path $PSScriptRoot "codex_window_dispatcher.ps1"

New-Item -ItemType Directory -Force $inbox | Out-Null

function Stop-WorkerByScriptName([string]$ScriptName) {
  $matches = Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -like "*$ScriptName*" }
  foreach ($match in $matches) {
    if ($match.ProcessId -ne $PID) {
      Stop-Process -Id $match.ProcessId -Force -ErrorAction SilentlyContinue
    }
  }
}

Stop-WorkerByScriptName "chat_inbox_bridge.ps1"
Stop-WorkerByScriptName "codex_window_dispatcher.ps1"

$bridge = Start-Process powershell -ArgumentList @(
  "-NoProfile",
  "-ExecutionPolicy", "Bypass",
  "-File", $bridgeScript,
  "-IntervalSeconds", [string]$IntervalSeconds
) -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $inbox "bridge.log") `
  -RedirectStandardError (Join-Path $inbox "bridge.err.log") `
  -PassThru

$dispatcher = Start-Process powershell -ArgumentList @(
  "-NoProfile",
  "-ExecutionPolicy", "Bypass",
  "-File", $dispatcherScript,
  "-IntervalSeconds", [string]$IntervalSeconds,
  "-Mode", $DispatcherMode,
  "-WindowTitle", $WindowTitle
) -WindowStyle Hidden `
  -RedirectStandardOutput (Join-Path $inbox "dispatcher.log") `
  -RedirectStandardError (Join-Path $inbox "dispatcher.err.log") `
  -PassThru

@{
  bridgePid = $bridge.Id
  dispatcherPid = $dispatcher.Id
  dispatcherMode = $DispatcherMode
  inbox = $inbox
} | ConvertTo-Json
