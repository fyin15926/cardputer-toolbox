param(
  [string]$InboxDir = ""
)

$ErrorActionPreference = "Stop"

if (!$InboxDir) {
  $InboxDir = Join-Path (Split-Path -Parent $PSScriptRoot) "codex-inbox"
}

$latestMessagePath = Join-Path $InboxDir "latest-message.json"
$latestChatPath = Join-Path $InboxDir "latest-chat.txt"
$pendingPath = Join-Path $InboxDir "pending-command.json"

if (Test-Path -LiteralPath $latestMessagePath) {
  $message = Get-Content -LiteralPath $latestMessagePath -Raw -Encoding UTF8 | ConvertFrom-Json
  Write-Output ("id={0}" -f $message.id)
  Write-Output ("time={0}" -f $message.time)
  Write-Output ("intent={0}" -f $message.intent)
  Write-Output ("requiresConfirmation={0}" -f $message.requiresConfirmation)
  Write-Output ""
  Write-Output ([string]$message.text)
} elseif (Test-Path -LiteralPath $latestChatPath) {
  Get-Content -LiteralPath $latestChatPath -Encoding UTF8
} else {
  Write-Output "No Cardputer inbox message found."
}

if (Test-Path -LiteralPath $pendingPath) {
  $pending = Get-Content -LiteralPath $pendingPath -Raw -Encoding UTF8 | ConvertFrom-Json
  Write-Output ""
  Write-Output ("pendingCommand={0}" -f $pending.id)
  Write-Output ("pendingIntent={0}" -f $pending.intent)
}
