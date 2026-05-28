$ErrorActionPreference = "Stop"

$names = @(
  "chat_inbox_bridge.ps1",
  "codex_window_dispatcher.ps1"
)

$stopped = @()
foreach ($name in $names) {
  $matches = Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -like "*$name*" }
  foreach ($match in $matches) {
    if ($match.ProcessId -ne $PID) {
      Stop-Process -Id $match.ProcessId -Force -ErrorAction SilentlyContinue
      $stopped += [ordered]@{
        script = $name
        processId = $match.ProcessId
      }
    }
  }
}

@{
  stopped = $stopped
} | ConvertTo-Json -Depth 4
