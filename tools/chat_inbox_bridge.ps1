param(
  [string]$Server = "http://cardputer.flye.cc",
  [string]$InboxDir = "",
  [string]$InboxPath = "",
  [string]$InboxPathFile = "C:\tmp\cardputer_chat_inbox_path.txt",
  [string]$TokenFile = "",
  [string]$NetConfig = "",
  [int]$IntervalSeconds = 2,
  [switch]$NoCommandQueue,
  [switch]$ForceLatest,
  [switch]$Once
)

$ErrorActionPreference = "Stop"

if (!$InboxDir) {
  $InboxDir = Join-Path (Split-Path -Parent $PSScriptRoot) "codex-inbox"
}

function Get-TokenFromNetConfig([string]$Path) {
  if (!$Path -or !(Test-Path -LiteralPath $Path)) { return "" }
  foreach ($line in Get-Content -LiteralPath $Path -ErrorAction Stop) {
    if ($line -match '^\s*token\s*=\s*(.+?)\s*$') {
      return $Matches[1].Trim()
    }
  }
  return ""
}

function Get-UploadToken {
  if ($env:CHAT_READ_TOKEN) { return $env:CHAT_READ_TOKEN.Trim() }
  if ($env:UPLOAD_TOKEN) { return $env:UPLOAD_TOKEN.Trim() }
  if ($TokenFile -and (Test-Path -LiteralPath $TokenFile)) {
    return (Get-Content -LiteralPath $TokenFile -Raw).Trim()
  }
  if ($NetConfig) {
    $fromNet = Get-TokenFromNetConfig $NetConfig
    if ($fromNet) { return $fromNet }
  }
  $commonNet = @(
    "F:\UPLOAD\net.txt",
    "E:\UPLOAD\net.txt",
    "D:\UPLOAD\net.txt"
  )
  foreach ($candidate in $commonNet) {
    $fromNet = Get-TokenFromNetConfig $candidate
    if ($fromNet) { return $fromNet }
  }
  return ""
}

function Get-ChatInboxPath {
  if ($InboxPath) { return $InboxPath.Trim() }
  if ($env:CHAT_INBOX_PATH) { return $env:CHAT_INBOX_PATH.Trim() }
  if ($InboxPathFile -and (Test-Path -LiteralPath $InboxPathFile)) {
    return (Get-Content -LiteralPath $InboxPathFile -Raw).Trim()
  }
  return ""
}

function Get-SafeFileId([string]$Value) {
  $safe = $Value -replace '[^\w.-]', '_'
  if (!$safe) { return "message" }
  return $safe
}

function New-Utf16Text([int[]]$Codes) {
  return -join ($Codes | ForEach-Object { [char]$_ })
}

function Test-ContainsAny([string]$Text, [string[]]$Words) {
  foreach ($word in $Words) {
    if ($word -and $Text.Contains($word)) { return $true }
  }
  return $false
}

function Get-CodexIntent([string]$Text) {
  $trimmed = $Text.Trim()
  $dangerWords = @(
    (New-Utf16Text @(0x5220,0x9664)),
    (New-Utf16Text @(0x6E05,0x7A7A)),
    (New-Utf16Text @(0x683C,0x5F0F,0x5316)),
    (New-Utf16Text @(0x91CD,0x7F6E)),
    (New-Utf16Text @(0x5173,0x673A)),
    (New-Utf16Text @(0x5378,0x8F7D)),
    (New-Utf16Text @(0x6CC4,0x9732)),
    (New-Utf16Text @(0x5BC6,0x7801)),
    (New-Utf16Text @(0x79C1,0x94A5)),
    "token", "rm -rf", "reset --hard", "format", "shutdown", "reboot"
  )
  $actors = @(
    "codex", "Codex",
    (New-Utf16Text @(0x5C0F,0x43)),
    (New-Utf16Text @(0x7535,0x8111)),
    (New-Utf16Text @(0x52A9,0x624B)),
    (New-Utf16Text @(0x4F60)),
    (New-Utf16Text @(0x5E2E,0x6211)),
    (New-Utf16Text @(0x5E2E,0x5FD9)),
    (New-Utf16Text @(0x8BF7)),
    (New-Utf16Text @(0x9EBB,0x70E6))
  )
  $verbs = @(
    (New-Utf16Text @(0x8FD0,0x884C)),
    (New-Utf16Text @(0x6267,0x884C)),
    (New-Utf16Text @(0x6253,0x5F00)),
    (New-Utf16Text @(0x4FEE,0x6539)),
    (New-Utf16Text @(0x5199)),
    (New-Utf16Text @(0x521B,0x5EFA)),
    (New-Utf16Text @(0x90E8,0x7F72)),
    (New-Utf16Text @(0x63D0,0x4EA4)),
    (New-Utf16Text @(0x8BFB,0x53D6)),
    (New-Utf16Text @(0x8BFB,0x4E00,0x4E0B)),
    (New-Utf16Text @(0x67E5,0x770B)),
    (New-Utf16Text @(0x770B,0x4E00,0x4E0B)),
    (New-Utf16Text @(0x68C0,0x67E5)),
    (New-Utf16Text @(0x6D4B,0x8BD5)),
    (New-Utf16Text @(0x7EE7,0x7EED)),
    (New-Utf16Text @(0x4FEE,0x590D)),
    (New-Utf16Text @(0x5B89,0x88C5)),
    (New-Utf16Text @(0x542F,0x52A8)),
    (New-Utf16Text @(0x505C,0x6B62)),
    (New-Utf16Text @(0x91CD,0x542F)),
    (New-Utf16Text @(0x641C,0x7D22)),
    (New-Utf16Text @(0x67E5,0x627E)),
    (New-Utf16Text @(0x63A7,0x5236)),
    (New-Utf16Text @(0x8FDE,0x63A5)),
    (New-Utf16Text @(0x540C,0x6B65))
  )
  $objects = @(
    (New-Utf16Text @(0x5C0F,0x673A,0x5668)),
    (New-Utf16Text @(0x6D88,0x606F)),
    (New-Utf16Text @(0x6587,0x4EF6)),
    (New-Utf16Text @(0x9879,0x76EE)),
    (New-Utf16Text @(0x4EE3,0x7801)),
    (New-Utf16Text @(0x670D,0x52A1)),
    (New-Utf16Text @(0x670D,0x52A1,0x5668)),
    (New-Utf16Text @(0x9875,0x9762)),
    (New-Utf16Text @(0x7F51,0x9875)),
    (New-Utf16Text @(0x65E5,0x5FD7)),
    (New-Utf16Text @(0x72B6,0x6001)),
    "inbox", "codex", "Codex"
  )

  if (Test-ContainsAny $trimmed $dangerWords) {
    return @{
      type = "dangerous"
      requiresConfirmation = $true
      reason = "matched dangerous action keywords"
    }
  }
  $hasActor = Test-ContainsAny $trimmed $actors
  $hasVerb = Test-ContainsAny $trimmed $verbs
  $hasObject = Test-ContainsAny $trimmed $objects
  if (($hasActor -and $hasVerb) -or ($hasVerb -and $hasObject)) {
    return @{
      type = "command"
      requiresConfirmation = $true
      reason = "matched command-like wording"
    }
  }
  return @{
    type = "chat"
    requiresConfirmation = $false
    reason = "plain chat message"
  }
}

function Write-CodexQueue($Job) {
  if ($NoCommandQueue) { return }

  $text = [string]$Job.userText
  if (!$text.Trim()) { return }

  $intent = Get-CodexIntent $text
  $safeId = Get-SafeFileId ([string]$Job.id)
  $stamp = [string]$Job.updatedAt
  if (!$stamp) { $stamp = [string]$Job.createdAt }

  $messagesDir = Join-Path $InboxDir "messages"
  $commandsDir = Join-Path $InboxDir "command-queue"
  $dangerDir = Join-Path $InboxDir "dangerous-queue"
  New-Item -ItemType Directory -Force $messagesDir | Out-Null
  New-Item -ItemType Directory -Force $commandsDir | Out-Null
  New-Item -ItemType Directory -Force $dangerDir | Out-Null

  $envelope = [ordered]@{
    id = [string]$Job.id
    time = $stamp
    deviceId = [string]$Job.deviceId
    text = $text
    intent = $intent.type
    requiresConfirmation = [bool]$intent.requiresConfirmation
    reason = [string]$intent.reason
    source = "cardputer-chat"
    createdAt = (Get-Date).ToUniversalTime().ToString("o")
    job = $Job
  }

  $messagePath = Join-Path $messagesDir "$safeId.json"
  ($envelope | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath $messagePath -Encoding UTF8
  ($envelope | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath (Join-Path $InboxDir "latest-message.json") -Encoding UTF8
  Set-Content -LiteralPath (Join-Path $InboxDir "latest-intent.txt") -Value ([string]$intent.type) -Encoding ASCII

  if ($intent.type -eq "command" -or $intent.type -eq "dangerous") {
    $queueDir = if ($intent.type -eq "dangerous") { $dangerDir } else { $commandsDir }
    $queuePath = Join-Path $queueDir "$safeId.json"
    ($envelope | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath $queuePath -Encoding UTF8
    ($envelope | ConvertTo-Json -Depth 10) | Set-Content -LiteralPath (Join-Path $InboxDir "pending-command.json") -Encoding UTF8
    $pendingText = @(
      "id=$($Job.id)"
      "time=$stamp"
      "intent=$($intent.type)"
      "requiresConfirmation=$($intent.requiresConfirmation)"
      ""
      $text
    ) -join "`r`n"
    Set-Content -LiteralPath (Join-Path $InboxDir "pending-command.txt") -Value $pendingText -Encoding UTF8
  }
}

function Write-ChatInbox($Job) {
  New-Item -ItemType Directory -Force $InboxDir | Out-Null
  $latestPath = Join-Path $InboxDir "latest-chat.txt"
  $latestJsonPath = Join-Path $InboxDir "latest-chat.json"
  $logPath = Join-Path $InboxDir "chat-log.jsonl"
  $lastIdPath = Join-Path $InboxDir "last-id.txt"

  $text = [string]$Job.userText
  $stamp = [string]$Job.updatedAt
  if (!$stamp) { $stamp = [string]$Job.createdAt }
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
  Write-CodexQueue $Job
}

function Get-LastSeenId {
  $path = Join-Path $InboxDir "last-id.txt"
  if (Test-Path -LiteralPath $path) {
    return (Get-Content -LiteralPath $path -Raw).Trim()
  }
  return ""
}

$Server = $Server.TrimEnd("/")
$chatInboxPath = Get-ChatInboxPath
$token = ""

if ($chatInboxPath) {
  if (!$chatInboxPath.StartsWith("/")) { $chatInboxPath = "/$chatInboxPath" }
} else {
  $token = Get-UploadToken
  if (!$token) {
    throw "CHAT_INBOX_PATH not found. Set `$env:CHAT_INBOX_PATH, pass -InboxPath, or save it in $InboxPathFile."
  }
}

function Invoke-ChatRecent {
  if ($chatInboxPath) {
    $uri = "$Server$chatInboxPath"
  } else {
    Add-Type -AssemblyName System.Web
    $encodedToken = [System.Web.HttpUtility]::UrlEncode($token)
    $uri = "$Server/api/chat/latest?token=$encodedToken"
  }
  $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
  if ($curl) {
    $tmp = Join-Path $env:TEMP ("cardputer-chat-{0}.json" -f ([guid]::NewGuid().ToString("N")))
    try {
      & curl.exe -sS --connect-timeout 10 --max-time 25 --url $uri --output $tmp
      if ($LASTEXITCODE -ne 0) {
        throw "curl.exe failed with exit code $LASTEXITCODE"
      }
      $raw = Get-Content -LiteralPath $tmp -Raw -Encoding UTF8
    } finally {
      Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
    return $raw | ConvertFrom-Json
  }
  return Invoke-RestMethod -Uri $uri -Method Get -TimeoutSec 20
}

Write-Host "Cardputer CHAT inbox bridge started."
Write-Host "Inbox: $InboxDir"
Write-Host "Server: $Server"
if ($chatInboxPath) {
  Write-Host "Read mode: inbox path"
} else {
  Write-Host "Read mode: token fallback"
}

do {
  try {
    $res = Invoke-ChatRecent
    $lastSeen = Get-LastSeenId
    $job = $res.job
    if ($job -and (!$job.userText -or !$job.id -or (!$ForceLatest -and $job.id -eq $lastSeen))) { $job = $null }
    if ($job) {
      Write-ChatInbox $job
      Write-Host ("[{0}] new chat: {1}" -f (Get-Date -Format "HH:mm:ss"), $job.id)
    }
  } catch {
    Write-Host ("[{0}] bridge error: {1}" -f (Get-Date -Format "HH:mm:ss"), $_.Exception.Message)
  }
  if (!$Once) { Start-Sleep -Seconds ([Math]::Max(1, $IntervalSeconds)) }
} while (!$Once)
