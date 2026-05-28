param(
  [string]$HostName = "root@47.110.91.244",
  [string]$KeyPath = "C:\tmp\cardputer_cloud_key",
  [string]$InboxDir = "",
  [int]$IntervalSeconds = 3,
  [switch]$Once
)

$ErrorActionPreference = "Stop"

if (!$InboxDir) {
  $InboxDir = Join-Path (Split-Path -Parent $PSScriptRoot) "codex-inbox"
}

$cacheDir = Join-Path $InboxDir "remote-chat-jobs"

function Sync-ChatJobs {
  New-Item -ItemType Directory -Force $cacheDir | Out-Null
  $remote = "${HostName}:/opt/cardputer-voice/chat-jobs/*.json"
  $dest = $cacheDir + "\"
  & scp -i $KeyPath $remote $dest
  if ($LASTEXITCODE -ne 0) {
    throw "scp failed with exit code $LASTEXITCODE"
  }
}

function Write-ChatInbox($Job, [string]$SourcePath) {
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
    "source=$SourcePath"
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

Write-Host "Cardputer CHAT SCP inbox bridge started."
Write-Host "Inbox: $InboxDir"
Write-Host "Host: $HostName"

do {
  try {
    Sync-ChatJobs
    $latest = Get-ChildItem -LiteralPath $cacheDir -Filter *.json |
      Sort-Object Name -Descending |
      Select-Object -First 1
    if ($latest) {
      $job = Get-Content -LiteralPath $latest.FullName -Raw | ConvertFrom-Json
      $lastSeen = Get-LastSeenId
      if ($job.id -and $job.userText -and $job.id -ne $lastSeen) {
        Write-ChatInbox $job $latest.FullName
        Write-Host ("[{0}] new chat: {1}" -f (Get-Date -Format "HH:mm:ss"), $job.id)
      }
    }
  } catch {
    Write-Host ("[{0}] bridge error: {1}" -f (Get-Date -Format "HH:mm:ss"), $_.Exception.Message)
  }
  if (!$Once) { Start-Sleep -Seconds ([Math]::Max(1, $IntervalSeconds)) }
} while (!$Once)
