param(
  [string]$InboxDir = "",
  [string]$WindowTitle = "Codex",
  [ValidateSet("Queue", "Paste", "Send")]
  [string]$Mode = "Queue",
  [int]$IntervalSeconds = 2,
  [switch]$ForceLatest,
  [switch]$Once
)

$ErrorActionPreference = "Stop"

if (!$InboxDir) {
  $InboxDir = Join-Path (Split-Path -Parent $PSScriptRoot) "codex-inbox"
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

function Get-ForegroundWindowTitle {
  if (-not ("Win32ForegroundWindow" -as [type])) {
    Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32ForegroundWindow {
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
}
"@
  }
  $handle = [Win32ForegroundWindow]::GetForegroundWindow()
  $buffer = New-Object System.Text.StringBuilder 512
  [void][Win32ForegroundWindow]::GetWindowText($handle, $buffer, $buffer.Capacity)
  return $buffer.ToString()
}

function Get-MessageId($Message) {
  return [string]$Message.id
}

function Get-DispatchedIdPath {
  return Join-Path $InboxDir "codex-window-dispatched-id.txt"
}

function Get-LastDispatchedId {
  $path = Get-DispatchedIdPath
  if (Test-Path -LiteralPath $path) {
    return (Get-Content -LiteralPath $path -Raw -Encoding UTF8).Trim()
  }
  return ""
}

function Set-LastDispatchedId([string]$Id) {
  Set-Content -LiteralPath (Get-DispatchedIdPath) -Value $Id -Encoding ASCII
}

function Normalize-CardputerText([string]$Text) {
  $result = $Text.Trim()
  $codex = "Codex"
  $pairs = @(
    @("Cox", $codex),
    @("cox", $codex),
    @("code x", $codex),
    @("Code x", $codex),
    @("code X", $codex),
    @("Code X", $codex),
    @("codex", $codex),
    @("CodeX", $codex)
  )
  foreach ($pair in $pairs) {
    $result = $result.Replace($pair[0], $pair[1])
  }
  return $result
}

function Test-ExplicitWindowDispatch([string]$Text) {
  $words = @(
    "codex", "Codex", "cox", "Cox",
    (New-Utf16Text @(0x5C0F,0x43)),
    (New-Utf16Text @(0x7535,0x8111)),
    (New-Utf16Text @(0x7A97,0x53E3)),
    (New-Utf16Text @(0x5BF9,0x8BDD,0x6846)),
    (New-Utf16Text @(0x53D1,0x7ED9)),
    (New-Utf16Text @(0x53D1,0x9001,0x7ED9)),
    (New-Utf16Text @(0x544A,0x8BC9)),
    (New-Utf16Text @(0x7EE7,0x7EED,0x9879,0x76EE))
  )
  return Test-ContainsAny $Text $words
}

function Build-CodexPrompt($Message) {
  $intent = [string]$Message.intent
  $id = [string]$Message.id
  $requiresConfirmation = [string]$Message.requiresConfirmation
  $normalizedText = Normalize-CardputerText ([string]$Message.text)
  $title = New-Utf16Text @(0x5C0F,0x673A,0x5668,0x8BED,0x97F3)
  $hint = New-Utf16Text @(0x8BF7,0x628A,0x4E0A,0x9762,0x5F53,0x4F5C,0x7528,0x6237,0x4ECE,0x5C0F,0x673A,0x5668,0x53D1,0x6765,0x7684,0x6D88,0x606F,0x5904,0x7406,0xFF1B,0x5371,0x9669,0x6216,0x4E0D,0x660E,0x786E,0x7684,0x52A8,0x4F5C,0x5148,0x786E,0x8BA4,0x3002)

  return @(
    "[$title]"
    "id: $id"
    "intent: $intent"
    "confirm: $requiresConfirmation"
    ""
    $normalizedText
    ""
    $hint
  ) -join "`r`n"
}

function Write-DispatchFiles($Message, [string]$Prompt, [string]$ModeUsed, [string]$Status) {
  New-Item -ItemType Directory -Force $InboxDir | Out-Null
  $id = Get-MessageId $Message
  $safeId = $id -replace '[^\w.-]', '_'
  if (!$safeId) { $safeId = "message" }
  $dispatchDir = Join-Path $InboxDir "codex-window-queue"
  New-Item -ItemType Directory -Force $dispatchDir | Out-Null

  $record = [ordered]@{
    id = $id
    time = [string]$Message.time
    intent = [string]$Message.intent
    mode = $ModeUsed
    status = $Status
    text = Normalize-CardputerText ([string]$Message.text)
    promptPath = "codex-inbox/codex-window-prompt.txt"
    createdAt = (Get-Date).ToUniversalTime().ToString("o")
  }

  Set-Content -LiteralPath (Join-Path $InboxDir "codex-window-prompt.txt") -Value $Prompt -Encoding UTF8
  ($record | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath (Join-Path $InboxDir "codex-window-last.json") -Encoding UTF8
  ($record | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath (Join-Path $dispatchDir "$safeId.json") -Encoding UTF8
}

function Invoke-CodexWindowPaste([string]$Prompt, [bool]$SendAfterPaste) {
  Set-Clipboard -Value $Prompt
  $shell = New-Object -ComObject WScript.Shell
  $activated = $shell.AppActivate($WindowTitle)
  if (!$activated) {
    throw "Could not activate a window matching '$WindowTitle'."
  }
  Start-Sleep -Milliseconds 250
  $activeTitle = Get-ForegroundWindowTitle
  if ($activeTitle -and $activeTitle -notlike "*$WindowTitle*") {
    throw "Active window is '$activeTitle', not '$WindowTitle'. Refusing to type."
  }
  $shell.SendKeys("^v")
  if ($SendAfterPaste) {
    Start-Sleep -Milliseconds 150
    $shell.SendKeys("{ENTER}")
  }
}

function Invoke-DispatchOnce {
  $messagePath = Join-Path $InboxDir "latest-message.json"
  if (!(Test-Path -LiteralPath $messagePath)) {
    Write-Host "No latest-message.json found."
    return
  }

  $message = Get-Content -LiteralPath $messagePath -Raw -Encoding UTF8 | ConvertFrom-Json
  $id = Get-MessageId $message
  if (!$id) {
    Write-Host "Latest message has no id."
    return
  }

  $lastId = Get-LastDispatchedId
  if (!$ForceLatest -and $id -eq $lastId) {
    Write-Host "No new message to dispatch: $id"
    return
  }

  $prompt = Build-CodexPrompt $message
  $text = [string]$message.text
  $intent = [string]$message.intent
  $allowWindowDispatch = $intent -ne "dangerous"
  $status = "queued"
  $modeUsed = $Mode

  if ($Mode -eq "Paste" -or $Mode -eq "Send") {
    if (!$allowWindowDispatch) {
      $status = "queued_dangerous_requires_review"
      $modeUsed = "Queue"
    } else {
      Invoke-CodexWindowPaste $prompt ($Mode -eq "Send")
      $status = if ($Mode -eq "Send") { "sent_to_codex_window" } else { "pasted_to_codex_window" }
    }
  }

  Write-DispatchFiles $message $prompt $modeUsed $status
  Set-LastDispatchedId $id
  Write-Host ("[{0}] {1}: {2}" -f (Get-Date -Format "HH:mm:ss"), $status, $id)
}

do {
  try {
    Invoke-DispatchOnce
  } catch {
    Write-Host ("[{0}] dispatcher error: {1}" -f (Get-Date -Format "HH:mm:ss"), $_.Exception.Message)
  }
  if (!$Once) { Start-Sleep -Seconds ([Math]::Max(1, $IntervalSeconds)) }
} while (!$Once)
