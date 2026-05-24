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
  logs/

/etc/cardputer-voice.env
/etc/systemd/system/cardputer-voice.service
/etc/nginx/conf.d/cardputer-voice-domain.conf
/etc/nginx/conf.d/cardputer-voice-31112.conf
```

## Environment

```text
PORT=3000
UPLOAD_TOKEN=replace-with-a-long-random-token
MAX_UPLOAD_BYTES=26214400
DATA_ROOT=/opt/cardputer-voice
```

`UPLOAD_TOKEN` must only live on the server and the Cardputer SD card config.

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
