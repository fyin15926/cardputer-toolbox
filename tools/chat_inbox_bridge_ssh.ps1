param(
  [string]$HostName = "root@47.110.91.244",
  [string]$KeyPath = "C:\tmp\cardputer_cloud_key",
  [string]$InboxDir = "",
  [int]$IntervalSeconds = 2,
  [switch]$Once
)

$ErrorActionPreference = "Stop"

if (!$InboxDir) {
  $InboxDir = Join-Path (Split-Path -Parent $PSScriptRoot) "codex-inbox"
}

function Invoke-Remote([string]$Command) {
  $sshExe = "$env:WINDIR\System32\OpenSSH\ssh.exe"
  if (!(Test-Path -LiteralPath $sshExe)) { $sshExe = "ssh" }
  $args = @(
    "-i", $KeyPath,
    "-o", "PreferredAuthentications=publickey",
    "-o", "IdentitiesOnly=yes",
    $HostName,
    $Command
  )
  $lastOutput = ""
  for ($attempt = 1; $attempt -le 5; $attempt++) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $sshExe @args 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    if ($exitCode -eq 0) {
      return ($output -join "`n")
    }
    $lastOutput = ($output -join "`n")
    Start-Sleep -Seconds 1
  }
  throw "ssh failed after retries: $lastOutput"
}

function Write-ChatInbox($Job) {
  New-Item -ItemType Directory -Force $InboxDir | Out-Null
  $latestPath = Join-Path $InboxDir "latest-chat.txt"
  $latestJsonPath = Join-Path $InboxDir "latest-chat.json"
  $logPath = Join-Path $InboxDir "chat-log.jsonl"
  $lastIdPath = Join-Path $InboxDir "last-id.txt"

  $stamp = [string]$Job.updatedAt
  if (!$stamp) { $stamp = [string]$Job.createdAt }
  $text = [string]$Job.userText
  $body = @(
    "id=$($Job.id)"
    "time=$stamp"
    "device=$($Job.deviceId)"
    ""
    $text
  ) -join "`r`n"

  Set-Content -LiteralPath $latestPath -Value $body -Encoding UTF8
  ($Job | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $latestJsonPath -Encoding UTF8
  ($Job | ConvertTo-Json -Compress -Depth 8) | Add-Content -LiteralPath $logPath -Encoding UTF8
  Set-Content -LiteralPath $lastIdPath -Value ([string]$Job.id) -Encoding ASCII
}

function Get-LastSeenId {
  $path = Join-Path $InboxDir "last-id.txt"
  if (Test-Path -LiteralPath $path) {
    return (Get-Content -LiteralPath $path -Raw).Trim()
  }
  return ""
}

Write-Host "Cardputer CHAT SSH inbox bridge started."
Write-Host "Inbox: $InboxDir"
Write-Host "Host: $HostName"

do {
  try {
    $latestPath = Invoke-Remote "ls -t /opt/cardputer-voice/chat-jobs/*.json 2>/dev/null | head -n 1"
    $latestPath = $latestPath.Trim()
    if ($latestPath) {
      $raw = Invoke-Remote "cat '$latestPath'"
      $job = $raw | ConvertFrom-Json
      $lastSeen = Get-LastSeenId
      if ($job.id -and $job.userText -and $job.id -ne $lastSeen) {
        Write-ChatInbox $job
        Write-Host ("[{0}] new chat: {1}" -f (Get-Date -Format "HH:mm:ss"), $job.id)
      }
    }
  } catch {
    Write-Host ("[{0}] bridge error: {1}" -f (Get-Date -Format "HH:mm:ss"), $_.Exception.Message)
  }
  if (!$Once) { Start-Sleep -Seconds ([Math]::Max(1, $IntervalSeconds)) }
} while (!$Once)
