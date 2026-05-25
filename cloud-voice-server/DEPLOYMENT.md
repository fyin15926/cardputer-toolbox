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
MAX_UPLOAD_BYTES=26214400
DATA_ROOT=/opt/cardputer-voice
TERMS_PATH=/opt/cardputer-voice/terms.json
```

`UPLOAD_TOKEN`, `ASR_FILE_TOKEN`, `DASHSCOPE_API_KEY`, and
`FLOMO_WEBHOOK_URL` must not be committed. `ASR_FILE_TOKEN` protects the
temporary public audio URL used by DashScope. Paraformer recorded-file
recognition requires a public HTTP/HTTPS file URL and does not accept local
file uploads or base64 audio.

## Safer nginx Setup

Use a dedicated host name so existing apps on `80` and `443` keep their own
`server_name` routing. The `31112` listener is only a fallback for device
testing when DNS is not ready.

```text
Primary after DNS:
  http://cardputer.flye.cc/upload

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
curl http://cardputer.flye.cc/jobs/REC_0001
```

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
curl -X POST http://cardputer.flye.cc/jobs/REC_0001/process
```

Resend an already transcribed memo to flomo after changing `terms.json` or the
memo format:

```bash
curl -X POST http://cardputer.flye.cc/jobs/REC_0001/resend
```
