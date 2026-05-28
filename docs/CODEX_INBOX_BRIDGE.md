# Cardputer to Codex Inbox Bridge

## Current Mode

The Cardputer `C` CHAT flow sends voice to the cloud server, the server runs ASR,
the server returns a short assistant reply for the Cardputer screen, and the
local bridge writes the latest message into `codex-inbox/` when the computer is
online.

This is intentionally an inbox bridge, not direct keyboard injection into the
Codex window. Direct injection is fragile because focus, ASR mistakes, or stale
text could send commands to the wrong place.

## Local Files

- `codex-inbox/latest-chat.txt`: human-readable latest message.
- `codex-inbox/latest-chat.json`: raw latest chat job from the server.
- `codex-inbox/latest-message.json`: local envelope with intent classification.
- `codex-inbox/latest-intent.txt`: `chat`, `command`, or `dangerous`.
- `codex-inbox/pending-command.txt`: latest command-like message, if any.
- `codex-inbox/pending-command.json`: structured pending command envelope.
- `codex-inbox/messages/`: archived message envelopes.
- `codex-inbox/command-queue/`: command-like messages.
- `codex-inbox/dangerous-queue/`: messages that must never run without review.

## Safety Rules

- The bridge never executes commands by itself.
- Command-like messages require Codex review.
- Dangerous words such as delete, reset, shutdown, token, password, or private
  key are routed to the dangerous queue.
- The cloud read path is a secret read-only inbox path. It does not expose upload
  permissions or dashboard access.
- The cloud chat reply is independent from the local Codex window. This lets the
  Cardputer talk to the assistant outdoors even when the computer window is not
  focused.
- Window dispatch is separate from inbox sync. The default mode is `Queue`; it
  only writes a Codex-ready prompt to `codex-inbox/codex-window-prompt.txt`.
- `Paste` and `Send` modes only target a window title matching `Codex`, and only
  when the spoken text contains an explicit window/Codex dispatch cue.

## Useful Commands

Run once:

```powershell
powershell -ExecutionPolicy Bypass -File tools\chat_inbox_bridge.ps1 -Once
```

Force reprocess latest message:

```powershell
powershell -ExecutionPolicy Bypass -File tools\chat_inbox_bridge.ps1 -Once -ForceLatest
```

Read the latest local inbox state:

```powershell
powershell -ExecutionPolicy Bypass -File tools\read_cardputer_inbox.ps1
```

Run continuously:

```powershell
powershell -ExecutionPolicy Bypass -File tools\chat_inbox_bridge.ps1 -IntervalSeconds 2
```

Start the full local control layer in safe queue mode:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\start_cardputer_codex_control.ps1 -DispatcherMode Queue
```

Start with window paste enabled:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\start_cardputer_codex_control.ps1 -DispatcherMode Paste
```

Start with automatic send enabled:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\start_cardputer_codex_control.ps1 -DispatcherMode Send
```

Stop the local control layer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\stop_cardputer_codex_control.ps1
```

## Voice Test Phrases

- Plain chat: `测试测试，现在是十六点二十一分。`
- Command-like: `帮我读取最新小机器消息。`
- Dangerous review path: `帮我删除这个项目。`
