'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { URL } = require('node:url');

const PORT = parseInt(process.env.PORT || '3000', 10);
const UPLOAD_TOKEN = process.env.UPLOAD_TOKEN || '';
const MAX_UPLOAD_BYTES = parseInt(process.env.MAX_UPLOAD_BYTES || `${25 * 1024 * 1024}`, 10);

const DATA_ROOT = process.env.DATA_ROOT
  ? path.resolve(process.env.DATA_ROOT)
  : __dirname;
const UPLOAD_DIR = path.join(DATA_ROOT, 'uploads');
const JOB_DIR = path.join(DATA_ROOT, 'jobs');

function sendJson(res, statusCode, payload) {
  const body = JSON.stringify(payload, null, 2);
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function timingSafeEqualText(a, b) {
  const left = Buffer.from(a || '', 'utf8');
  const right = Buffer.from(b || '', 'utf8');
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function sanitizeRecordingName(name) {
  const fallback = `REC_${new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14)}.wav`;
  const base = path.basename(String(name || fallback)).replace(/[^\w.-]/g, '_');
  const candidate = base || fallback;
  return candidate.toLowerCase().endsWith('.wav') ? candidate : `${candidate}.wav`;
}

function makeJobId(deviceId, recordingName) {
  const stamp = new Date().toISOString().replace(/[-:.TZ]/g, '').slice(0, 14);
  const safeDevice = String(deviceId || 'unknown').replace(/[^\w.-]/g, '_').slice(0, 40);
  const safeName = path.basename(recordingName, '.wav').replace(/[^\w.-]/g, '_').slice(0, 80);
  const suffix = crypto.randomBytes(3).toString('hex');
  return `${stamp}_${safeDevice}_${safeName}_${suffix}`;
}

async function ensureRuntimeDirs() {
  await Promise.all([
    fsp.mkdir(UPLOAD_DIR, { recursive: true }),
    fsp.mkdir(JOB_DIR, { recursive: true })
  ]);
}

async function handleHealth(_req, res) {
  sendJson(res, 200, {
    ok: true,
    service: 'cardputer-cloud-voice-server',
    phase: 1,
    time: new Date().toISOString()
  });
}

async function handleUpload(req, res) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }

  if (!timingSafeEqualText(req.headers['x-upload-token'], UPLOAD_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const contentType = String(req.headers['content-type'] || '').toLowerCase();
  if (!contentType.includes('audio/wav') && !contentType.includes('audio/x-wav')) {
    sendJson(res, 415, { ok: false, error: 'Content-Type must be audio/wav' });
    return;
  }

  const deviceId = String(req.headers['x-device-id'] || 'unknown').slice(0, 80);
  const recordingName = sanitizeRecordingName(req.headers['x-recording-name']);
  const jobId = makeJobId(deviceId, recordingName);
  const uploadPath = path.join(UPLOAD_DIR, `${jobId}_${recordingName}`);
  const jobPath = path.join(JOB_DIR, `${jobId}.json`);
  const startedAt = new Date().toISOString();

  let bytes = 0;
  const out = fs.createWriteStream(uploadPath, { flags: 'wx' });

  req.on('data', (chunk) => {
    bytes += chunk.length;
    if (bytes > MAX_UPLOAD_BYTES) {
      req.destroy(new Error('upload too large'));
    }
  });

  req.pipe(out);

  try {
    await new Promise((resolve, reject) => {
      req.on('error', reject);
      out.on('error', reject);
      out.on('finish', resolve);
    });

    const job = {
      id: jobId,
      status: 'uploaded',
      phase: 1,
      deviceId,
      recordingName,
      bytes,
      uploadPath,
      createdAt: startedAt,
      updatedAt: new Date().toISOString()
    };

    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');
    sendJson(res, 201, { ok: true, job });
  } catch (error) {
    out.destroy();
    await fsp.rm(uploadPath, { force: true }).catch(() => {});
    const statusCode = error.message === 'upload too large' ? 413 : 500;
    sendJson(res, statusCode, { ok: false, error: error.message });
  }
}

async function handleJob(req, res, pathname) {
  const id = decodeURIComponent(pathname.slice('/jobs/'.length));
  if (!/^[\w.-]+$/.test(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  const jobPath = path.join(JOB_DIR, `${id}.json`);
  try {
    const raw = await fsp.readFile(jobPath, 'utf8');
    sendJson(res, 200, { ok: true, job: JSON.parse(raw) });
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'job not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function route(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  const pathname = url.pathname;

  if (req.method === 'GET' && pathname === '/health') {
    await handleHealth(req, res);
    return;
  }
  if (req.method === 'POST' && pathname === '/upload') {
    await handleUpload(req, res);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/jobs/')) {
    await handleJob(req, res, pathname);
    return;
  }

  sendJson(res, 404, { ok: false, error: 'not found' });
}

async function main() {
  await ensureRuntimeDirs();

  const server = http.createServer((req, res) => {
    route(req, res).catch((error) => {
      sendJson(res, 500, { ok: false, error: error.message });
    });
  });

  server.listen(PORT, () => {
    console.log(`Cardputer cloud voice server listening on ${PORT}`);
    console.log(`Data root: ${DATA_ROOT}`);
  });
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
