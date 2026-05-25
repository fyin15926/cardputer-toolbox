# Cardputer Cloud Voice Server

Cloud service for receiving Cardputer WAV recordings, transcribing them with
Alibaba Cloud DashScope Paraformer, and sending the transcript to flomo.

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
- `GET /jobs`: lists recent jobs. Requires `X-Upload-Token`; supports `limit=20` and `status=done`.
- `GET /jobs/:id`: returns saved upload metadata.
- `POST /jobs/:id/process`: manually queues a saved job for transcription.
- `POST /jobs/:id/resend`: resends an already transcribed job to flomo using the current memo format and `terms.json`.
- `GET /audio/:recordingName?token=...`: private audio URL for DashScope fetch.

Production deployment notes and nginx/systemd templates are in
`DEPLOYMENT.md` and `deploy/`.

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

Successful response:

```json
{
  "ok": true,
  "id": "REC_0123"
}
```

Runtime output is written as `uploads/REC_0123.wav` and `jobs/REC_0123.json`.
These directories are intentionally ignored by Git.

Check recent jobs without exposing transcripts or private audio URLs:

```bash
curl http://127.0.0.1:3000/jobs?limit=20 \
  -H "X-Upload-Token: change-me-to-a-long-random-token"
```

Filter by status when debugging:

```bash
curl "http://127.0.0.1:3000/jobs?status=flomo_failed&limit=10" \
  -H "X-Upload-Token: change-me-to-a-long-random-token"
```

When transcription is configured, raw text is written to
`transcripts/REC_0123.txt`, the formatted flomo memo is written to
`transcripts/REC_0123.memo.txt`, and the memo is sent to flomo.

Optional project-specific text corrections can be placed in `terms.json` next
to `server.js` or at `TERMS_PATH`. Use `terms.example.json` as a starting
point. The file may be either an array of pairs or an object:

```json
[
  ["浮墨", "flomo"],
  ["card puter", "Cardputer"]
]
```
