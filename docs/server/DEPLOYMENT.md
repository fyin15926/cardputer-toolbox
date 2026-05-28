# Cardputer Voice Server Deployment

This service is deployed as an isolated Node.js systemd service behind nginx.
Do not commit real tokens or production `.env` files.

## Runtime Layout

```text
/opt/cardputer-voice/
  server.js
  package.json
  uploads/
  jobs/
  transcripts/
  logs/
  terms.json              # optional correction dictionary

/etc/cardputer-voice.env
/etc/systemd/system/cardputer-voice.service
/etc/nginx/conf.d/cardputer-voice-domain.conf
/etc/nginx/conf.d/cardputer-voice-31112.conf
```

## Environment

```text
PORT=3000
UPLOAD_TOKEN=replace-with-a-long-random-token
PUBLIC_BASE_URL=http://cardputer.flye.cc
ASR_FILE_TOKEN=replace-with-a-different-long-random-token
DASHSCOPE_API_KEY=sk-...
FLOMO_WEBHOOK_URL=https://flomoapp.com/iwh/...
DEEPSEEK_API_KEY=sk-...
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-v4-pro
DEEPSEEK_TIMEOUT_MS=45000
DEEPSEEK_MAX_TRANSCRIPT_CHARS=20000
CHAT_REPLY_ENABLED=true
CHAT_CONTEXT_TURNS=4
CHAT_TTS_ENABLED=true
DASH_SCOPE_TTS_MODEL=cosyvoice-v3-flash
DASH_SCOPE_TTS_VOICE=longanyang
DASH_SCOPE_TTS_SAMPLE_RATE=24000
CHAT_TTS_SETTINGS_PATH=/opt/cardputer-voice/chat-tts-settings.json
MAX_UPLOAD_BYTES=67108864
DATA_ROOT=/opt/cardputer-voice
TERMS_PATH=/opt/cardputer-voice/terms.json
DEEPSEEK_SETTINGS_PATH=/opt/cardputer-voice/deepseek-settings.json
```

`UPLOAD_TOKEN`, `ASR_FILE_TOKEN`, `DASHSCOPE_API_KEY`, `DEEPSEEK_API_KEY`, and
`FLOMO_WEBHOOK_URL` must not be committed. `ASR_FILE_TOKEN` protects the
temporary public audio URL used by DashScope. Paraformer recorded-file
recognition requires a public HTTP/HTTPS file URL and does not accept local
file uploads or base64 audio.

`DEEPSEEK_SETTINGS_PATH` is optional. If omitted, the dashboard saves editable
DeepSeek prompt/settings to `DATA_ROOT/deepseek-settings.json`.

`CHAT_REPLY_ENABLED=true` lets `/chat/upload` return a short assistant reply for
the Cardputer screen after ASR. If the model call fails, the server falls back to
the old `I heard you` style response so voice input still works. `CHAT_CONTEXT_TURNS`
controls how many recent Cardputer chat turns are sent as lightweight context.

`CHAT_TTS_ENABLED=true` asks DashScope TTS to synthesize the assistant reply as
24 kHz mono PCM wrapped in WAV. The server returns the WAV URL as `audioUrl` and
serves it from `/chat-tts/` with the private ASR audio token.

CHAT also supports a spoken TTS voice switch. Saying `切换成粤语` or `切换成广东话`
sets the current voice to `longanyue_v3`; saying `切换成女声` or `切换成龙安雅`
sets it to `longanya_v3`; saying `切换成男声` or `切换成龙安洋` sets it to
`longanyang`; saying `切换成普通话` or `切换成国语` sets it back to
`DASH_SCOPE_TTS_VOICE`. The current choice is persisted in `CHAT_TTS_SETTINGS_PATH`,
so it survives service restarts.

For longer recordings, keep nginx `client_max_body_size` and
`MAX_UPLOAD_BYTES` aligned. The current template allows 64 MB uploads
(`MAX_UPLOAD_BYTES=67108864`), enough for roughly 30+ minutes of 16 kHz mono
WAV.

## Safer nginx Setup

Use a dedicated host name so existing apps on `80` and `443` keep their own
`server_name` routing. The `31112` listener is only a fallback for device
testing when DNS is not ready.

```text
Primary after DNS:
  http://cardputer.flye.cc/upload
  http://cardputer.flye.cc/dashboard
  http://cardputer.flye.cc/chat/upload
  http://cardputer.flye.cc/inbox/<long-random-value>

Fallback:
  http://47.110.91.244:31112/upload
```

Point `cardputer.flye.cc` to the ECS public IP before switching devices to the
primary URL.

## Checks

```bash
systemctl status cardputer-voice
journalctl -u cardputer-voice -f
nginx -t
curl http://127.0.0.1:3000/health
curl -H 'Host: cardputer.flye.cc' http://127.0.0.1/health
curl -H "X-Upload-Token: $UPLOAD_TOKEN" 'http://cardputer.flye.cc/jobs?limit=20'
curl -H "X-Upload-Token: $UPLOAD_TOKEN" 'http://cardputer.flye.cc/api/dashboard?limit=20'
curl http://cardputer.flye.cc/jobs/REC_0001
curl -i -X POST http://cardputer.flye.cc/chat/upload \
  -H "Content-Type: audio/wav" \
  --data-binary @REC_0001.wav
```

The unauthenticated `/chat/upload` check should return JSON `401 invalid upload
token` from Node. If it returns an nginx HTML 404, the nginx CHAT routes are
missing.

For the local Codex bridge, configure a long unguessable `CHAT_INBOX_PATH` such
as `/inbox/<long-random-value>` on the server and save the same path on the
computer in `C:\tmp\cardputer_chat_inbox_path.txt`. This reads only the latest
chat text and avoids putting the upload token in a header or query string.

Upload smoke test:

```bash
curl -X POST http://cardputer.flye.cc/upload \
  -H "X-Upload-Token: $UPLOAD_TOKEN" \
  -H "X-Device-Id: cardputer-001" \
  -H "X-Recording-Name: REC_0001.wav" \
  -H "Content-Type: audio/wav" \
  --data-binary @REC_0001.wav
```

Manual processing:

```bash
curl -X POST http://cardputer.flye.cc/jobs/REC_0001/process \
  -H "X-Upload-Token: $UPLOAD_TOKEN"
```

Resend an already transcribed memo to flomo after changing `terms.json` or the
memo format:

```bash
curl -X POST http://cardputer.flye.cc/jobs/REC_0001/resend \
  -H "X-Upload-Token: $UPLOAD_TOKEN"
```
