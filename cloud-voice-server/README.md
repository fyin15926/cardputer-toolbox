# Cardputer Cloud Voice Server

Phase 1 minimal server for receiving Cardputer WAV recordings.

## Run

```powershell
cd D:\github仓库同步\小机器\cloud-voice-server
$env:UPLOAD_TOKEN="change-me-to-a-long-random-token"
npm start
```

Or on Linux:

```bash
cd /opt/cardputer-voice
export UPLOAD_TOKEN='change-me-to-a-long-random-token'
npm start
```

## Endpoints

- `GET /health`: health check.
- `POST /upload`: accepts raw `audio/wav` bytes with `X-Upload-Token`.
- `GET /jobs/:id`: returns saved upload metadata.

Upload headers:

```text
X-Upload-Token: your token
X-Device-Id: cardputer-001
X-Recording-Name: REC_0123.wav
Content-Type: audio/wav
```

Test upload:

```bash
curl -X POST http://127.0.0.1:3000/upload \
  -H "X-Upload-Token: change-me-to-a-long-random-token" \
  -H "X-Device-Id: cardputer-001" \
  -H "X-Recording-Name: REC_0123.wav" \
  -H "Content-Type: audio/wav" \
  --data-binary @REC_0123.wav
```

Runtime output is written to `uploads/` and `jobs/`. These directories are intentionally ignored by Git.
