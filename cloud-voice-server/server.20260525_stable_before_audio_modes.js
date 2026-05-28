'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { URL } = require('node:url');

const PORT = parseInt(process.env.PORT || '3000', 10);
const UPLOAD_TOKEN = process.env.UPLOAD_TOKEN || '';
const ASR_FILE_TOKEN = process.env.ASR_FILE_TOKEN || '';
const DASHSCOPE_API_KEY = process.env.DASHSCOPE_API_KEY || '';
const FLOMO_WEBHOOK_URL = process.env.FLOMO_WEBHOOK_URL || '';
const PUBLIC_BASE_URL = (process.env.PUBLIC_BASE_URL || '').replace(/\/+$/, '');
const MAX_UPLOAD_BYTES = parseInt(process.env.MAX_UPLOAD_BYTES || `${64 * 1024 * 1024}`, 10);
const DASH_SCOPE_TRANSCRIPTION_URL = 'https://dashscope.aliyuncs.com/api/v1/services/audio/asr/transcription';
const DASH_SCOPE_TASK_URL = 'https://dashscope.aliyuncs.com/api/v1/tasks/';

const DATA_ROOT = process.env.DATA_ROOT
  ? path.resolve(process.env.DATA_ROOT)
  : __dirname;
const UPLOAD_DIR = path.join(DATA_ROOT, 'uploads');
const JOB_DIR = path.join(DATA_ROOT, 'jobs');
const TRANSCRIPT_DIR = path.join(DATA_ROOT, 'transcripts');
const PREVIEW_DIR = path.join(DATA_ROOT, 'previews');
const TERMS_PATH = process.env.TERMS_PATH
  ? path.resolve(process.env.TERMS_PATH)
  : path.join(DATA_ROOT, 'terms.json');
const processingJobs = new Set();
const activeUploads = new Map();
const deviceStatuses = new Map();

const DEFAULT_PREVIEW_PARAMS = {
  gain: 1,
  lowpass: 48,
  highMix: 0.72,
  scratchRmsMax: 1200,
  scratchDiffMin: 260,
  scratchRatio: 145,
  holdFrames: 0,
  frameSamples: 256,
  noiseRmsMax: 420,
  noiseMix: 0.78
};

const DEFAULT_ASR_PARAMS = {
  gain: 1.25,
  highpass: 0.985,
  preEmphasis: 0.18,
  noiseGateRms: 520,
  noiseGateFloor: 0.18,
  compressorThreshold: 5200,
  compressorRatio: 3.2,
  targetPeak: 26000,
  limiter: 30000,
  frameSamples: 320
};

function sendJson(res, statusCode, payload) {
  const body = JSON.stringify(payload, null, 2);
  res.writeHead(statusCode, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function sendHtml(res, statusCode, body) {
  res.writeHead(statusCode, {
    'Content-Type': 'text/html; charset=utf-8',
    'Content-Length': Buffer.byteLength(body)
  });
  res.end(body);
}

function redirect(res, location) {
  res.writeHead(302, {
    Location: location,
    'Cache-Control': 'no-store'
  });
  res.end();
}

function timingSafeEqualText(a, b) {
  const left = Buffer.from(a || '', 'utf8');
  const right = Buffer.from(b || '', 'utf8');
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function hasValidUploadToken(req) {
  return Boolean(UPLOAD_TOKEN) && timingSafeEqualText(req.headers['x-upload-token'], UPLOAD_TOKEN);
}

function normalizeRecordingName(name) {
  const raw = String(name || '').trim();
  const base = path.basename(raw).replace(/[^\w.-]/g, '_');
  if (!base || base === '.' || base === '..') {
    return null;
  }

  const withoutExt = base.toLowerCase().endsWith('.wav') ? base.slice(0, -4) : base;
  const id = withoutExt.replace(/[^\w.-]/g, '_').slice(0, 80);
  if (!id || id === '.' || id === '..') {
    return null;
  }

  return {
    id,
    recordingName: `${id}.wav`
  };
}

async function ensureRuntimeDirs() {
  await Promise.all([
    fsp.mkdir(UPLOAD_DIR, { recursive: true }),
    fsp.mkdir(JOB_DIR, { recursive: true }),
    fsp.mkdir(TRANSCRIPT_DIR, { recursive: true }),
    fsp.mkdir(PREVIEW_DIR, { recursive: true })
  ]);
}

async function handleHealth(_req, res) {
  sendJson(res, 200, {
    ok: true,
    service: 'cardputer-cloud-voice-server',
    phase: 4,
    configured: {
      uploadToken: Boolean(UPLOAD_TOKEN),
      publicBaseUrl: Boolean(PUBLIC_BASE_URL),
      asrFileToken: Boolean(ASR_FILE_TOKEN),
      dashScope: Boolean(DASHSCOPE_API_KEY),
      flomo: Boolean(FLOMO_WEBHOOK_URL)
    },
    time: new Date().toISOString()
  });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function jobPathForId(id) {
  return path.join(JOB_DIR, `${id}.json`);
}

function transcriptMemoPathForId(id) {
  return path.join(TRANSCRIPT_DIR, `${id}.memo.txt`);
}

function previewPathForId(id) {
  return path.join(PREVIEW_DIR, `${id}.play-preview.wav`);
}

function previewMetaPathForId(id) {
  return path.join(PREVIEW_DIR, `${id}.play-preview.json`);
}

function previewFeedbackPathForId(id) {
  return path.join(PREVIEW_DIR, `${id}.feedback.json`);
}

function asrCleanPathForId(id) {
  return path.join(PREVIEW_DIR, `${id}.clean-for-asr.wav`);
}

function asrCleanMetaPathForId(id) {
  return path.join(PREVIEW_DIR, `${id}.clean-for-asr.json`);
}

function audioPathForJob(job) {
  return job.uploadPath ? resolveDataPath(job.uploadPath) : path.join(UPLOAD_DIR, job.recordingName || '');
}

async function readJob(id) {
  const raw = await fsp.readFile(jobPathForId(id), 'utf8');
  return JSON.parse(raw);
}

async function writeJob(job) {
  job.updatedAt = new Date().toISOString();
  await fsp.writeFile(jobPathForId(job.id), `${JSON.stringify(job, null, 2)}\n`, 'utf8');
}

function summarizeJob(job) {
  return {
    id: job.id,
    status: job.status,
    phase: job.phase,
    deviceId: job.deviceId,
    recordingName: job.recordingName,
    recordedAt: job.recordedAt,
    bytes: job.bytes,
    uploadEncoding: job.uploadEncoding,
    uploadedBytes: job.uploadedBytes,
    originalBytes: job.originalBytes,
    compressionRatio: job.compressionRatio,
    createdAt: job.createdAt,
    updatedAt: job.updatedAt,
    pendingReason: job.pendingReason,
    lastError: job.lastError,
    memo: job.memo,
    flomo: job.flomo
      ? {
          sentAt: job.flomo.sentAt,
          resentAt: job.flomo.resentAt,
          memoSlug: job.flomo.memoSlug
        }
      : undefined
  };
}

function updateDeviceStatus(deviceId, patch) {
  if (!deviceId) return;
  const previous = deviceStatuses.get(deviceId) || { deviceId };
  deviceStatuses.set(deviceId, {
    ...previous,
    ...patch,
    deviceId,
    updatedAt: new Date().toISOString()
  });
}

function parseHeaderInt(value) {
  const n = parseInt(String(value || '').trim(), 10);
  return Number.isFinite(n) ? n : undefined;
}

const IMA_INDEX_TABLE = [
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
];
const IMA_STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
  19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
];

function clampInt16(n) {
  return Math.max(-32768, Math.min(32767, n));
}

function writeWavHeaderBuffer(target, sampleRate, dataBytes) {
  const byteRate = sampleRate * 2;
  target.write('RIFF', 0, 'ascii');
  target.writeUInt32LE(36 + dataBytes, 4);
  target.write('WAVE', 8, 'ascii');
  target.write('fmt ', 12, 'ascii');
  target.writeUInt32LE(16, 16);
  target.writeUInt16LE(1, 20);
  target.writeUInt16LE(1, 22);
  target.writeUInt32LE(sampleRate, 24);
  target.writeUInt32LE(byteRate, 28);
  target.writeUInt16LE(2, 32);
  target.writeUInt16LE(16, 34);
  target.write('data', 36, 'ascii');
  target.writeUInt32LE(dataBytes, 40);
}

function decodeImaNibble(nibble, state) {
  let step = IMA_STEP_TABLE[state.index];
  let diff = step >> 3;
  if (nibble & 1) diff += step >> 2;
  if (nibble & 2) diff += step >> 1;
  if (nibble & 4) diff += step;
  state.predictor = clampInt16((nibble & 8) ? state.predictor - diff : state.predictor + diff);
  state.index = Math.max(0, Math.min(88, state.index + IMA_INDEX_TABLE[nibble & 0x0f]));
  return state.predictor;
}

function decodeImaAdpcmToWav(encoded, { sampleRate, pcmBytes, predictor, index }) {
  if (!Number.isFinite(sampleRate) || sampleRate < 8000 || sampleRate > 48000) {
    throw new Error('invalid ADPCM sample rate');
  }
  if (!Number.isFinite(pcmBytes) || pcmBytes < 2 || pcmBytes > MAX_UPLOAD_BYTES) {
    throw new Error('invalid ADPCM PCM size');
  }
  if (pcmBytes % 2 !== 0) {
    throw new Error('ADPCM PCM size must be even');
  }
  if (!Number.isFinite(predictor) || !Number.isFinite(index) || index < 0 || index > 88) {
    throw new Error('invalid ADPCM state');
  }

  const output = Buffer.alloc(44 + pcmBytes);
  writeWavHeaderBuffer(output, sampleRate, pcmBytes);
  const state = { predictor: clampInt16(predictor), index };
  let offset = 44;
  output.writeInt16LE(state.predictor, offset);
  offset += 2;
  const sampleCount = pcmBytes / 2;
  let writtenSamples = 1;
  for (const byte of encoded) {
    for (const nibble of [byte & 0x0f, (byte >> 4) & 0x0f]) {
      if (writtenSamples >= sampleCount) break;
      output.writeInt16LE(decodeImaNibble(nibble, state), offset);
      offset += 2;
      writtenSamples++;
    }
    if (writtenSamples >= sampleCount) break;
  }
  if (writtenSamples !== sampleCount) {
    throw new Error('incomplete ADPCM body');
  }
  return output;
}

function collectRequestBody(req, maxBytes, onProgress) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let total = 0;
    req.on('data', (chunk) => {
      total += chunk.length;
      if (onProgress) onProgress(total);
      if (total > maxBytes) {
        req.destroy(new Error('upload too large'));
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks, total)));
    req.on('error', reject);
  });
}

async function readJsonBody(req, maxBytes = 16 * 1024) {
  const body = await collectRequestBody(req, maxBytes);
  if (!body.length) return {};
  return JSON.parse(body.toString('utf8'));
}

function dashboardAuth(req, res) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return false;
  }
  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return false;
  }
  return true;
}

async function listJobs({ limit = 20, status = '' } = {}) {
  const entries = await fsp.readdir(JOB_DIR, { withFileTypes: true });
  const jobs = [];
  const statusCounts = {};
  let skipped = 0;

  await Promise.all(entries.map(async (entry) => {
    if (!entry.isFile() || !entry.name.endsWith('.json')) {
      return;
    }
    try {
      const raw = await fsp.readFile(path.join(JOB_DIR, entry.name), 'utf8');
      const job = JSON.parse(raw);
      const jobStatus = String(job.status || 'unknown');
      statusCounts[jobStatus] = (statusCounts[jobStatus] || 0) + 1;
      if (!status || (status === 'failed' ? jobStatus.includes('failed') : jobStatus === status)) {
        jobs.push(summarizeJob(job));
      }
    } catch {
      skipped++;
    }
  }));

  jobs.sort((a, b) => {
    const left = Date.parse(a.updatedAt || a.createdAt || '') || 0;
    const right = Date.parse(b.updatedAt || b.createdAt || '') || 0;
    return right - left;
  });

  return {
    count: Math.min(jobs.length, limit),
    total: jobs.length,
    totalAll: Object.values(statusCounts).reduce((sum, count) => sum + count, 0),
    statusCounts,
    skipped,
    jobs: jobs.slice(0, limit)
  };
}

function dashboardHtml() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cardputer Voice Dashboard</title>
  <style>
    :root { color-scheme: dark; --bg:#070b09; --panel:#101613; --soft:#0c120f; --line:#21412f; --text:#e8fff0; --muted:#8fb09b; --ok:#40ff83; --bad:#ff5d5d; --warn:#ffd166; }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }
    header { border-bottom: 1px solid var(--line); background: #090f0c; position: sticky; top: 0; z-index: 2; }
    .head { max-width: 1200px; margin: 0 auto; padding: 18px; }
    h1 { margin: 0 0 12px; font-size: 22px; font-weight: 750; letter-spacing: 0; }
    .bar { display: grid; grid-template-columns: minmax(220px, 1fr) auto auto auto auto; gap: 8px; align-items: center; }
    input, button, select { height: 38px; border: 1px solid var(--line); background: var(--soft); color: var(--text); border-radius: 6px; padding: 0 10px; font: inherit; min-width: 0; }
    button { cursor: pointer; color: var(--ok); white-space: nowrap; }
    button:hover { border-color: var(--ok); }
    button.primary { color: #061009; background: var(--ok); border-color: var(--ok); font-weight: 700; }
    button.ghost { color: var(--muted); }
    button.danger { color: var(--bad); }
    button.small { height: 28px; padding: 0 8px; font-size: 12px; }
    main { padding: 16px 18px 24px; max-width: 1180px; margin: 0 auto; }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; margin-bottom: 14px; }
    .panel { border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 12px; }
    .label { color: var(--muted); font-size: 12px; margin-bottom: 6px; }
    .value { font-size: 22px; font-weight: 700; overflow-wrap: anywhere; }
    .ok { color: var(--ok); }
    .bad { color: var(--bad); }
    .warn { color: var(--warn); }
    section { margin-top: 14px; }
    h2 { font-size: 15px; margin: 0 0 10px; color: var(--ok); }
    table { width: 100%; border-collapse: collapse; font-size: 13px; }
    th, td { border-bottom: 1px solid #17251d; padding: 8px 6px; text-align: left; vertical-align: top; }
    th { color: var(--muted); font-weight: 600; }
    .pill { display: inline-block; border: 1px solid var(--line); border-radius: 999px; padding: 2px 7px; color: var(--ok); white-space: nowrap; }
    .pill.bad { color: var(--bad); }
    .pill.warn { color: var(--warn); }
    .progress { height: 8px; background: #07100a; border: 1px solid var(--line); border-radius: 999px; overflow: hidden; min-width: 120px; }
    .fill { height: 100%; width: 0; background: var(--ok); }
    .muted { color: var(--muted); }
    .statusline { display: flex; gap: 10px; align-items: center; min-height: 24px; margin-top: 8px; color: var(--muted); font-size: 13px; white-space: normal; word-break: normal; writing-mode: horizontal-tb; flex-wrap: wrap; }
    .error { color: var(--bad); white-space: nowrap; word-break: normal; writing-mode: horizontal-tb; display: inline-block; }
    .notice { display: flex; justify-content: space-between; gap: 12px; align-items: center; margin-bottom: 14px; }
    .hidden { display: none !important; }
    .section-title { display: flex; justify-content: space-between; gap: 10px; align-items: center; margin-bottom: 10px; }
    .section-title h2 { margin: 0; }
    .actions { display: flex; gap: 6px; flex-wrap: wrap; }
    .filters { display: flex; gap: 6px; flex-wrap: wrap; justify-content: flex-end; align-items: center; }
    .filters button { height: 30px; padding: 0 9px; font-size: 12px; color: var(--muted); }
    .filters button.active { color: var(--ok); border-color: var(--ok); background: #0f1f16; }
    a.button { display: inline-flex; align-items: center; height: 28px; padding: 0 8px; border: 1px solid var(--line); border-radius: 6px; color: var(--ok); text-decoration: none; font-size: 12px; white-space: nowrap; }
    a.button:hover { border-color: var(--ok); }
    .nowrap { white-space: nowrap; }
    @media (max-width: 900px) { .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .bar { grid-template-columns: 1fr auto auto; } #limit, #clear { grid-column: auto; } table { font-size: 12px; } }
    @media (max-width: 560px) { .grid { grid-template-columns: 1fr; } .bar { grid-template-columns: 1fr 1fr; } #token { grid-column: 1 / -1; } }
  </style>
</head>
<body>
  <header>
    <div class="head">
      <h1>Cardputer Voice Dashboard</h1>
      <div class="statusline" style="margin:-6px 0 12px"><a class="button" href="/dashboard/lab">打开音频实验室</a></div>
      <div class="bar">
        <input id="token" type="password" autocomplete="off" placeholder="UPLOAD_TOKEN">
        <button id="save">保存</button>
        <button id="refresh">刷新</button>
        <button id="clear" class="danger">清除</button>
        <select id="limit"><option>20</option><option selected>50</option><option>100</option></select>
      </div>
      <div class="statusline"><span id="stamp">等待登录</span><span id="error" class="error"></span></div>
    </div>
  </header>
  <main>
    <div id="loginHint" class="panel notice">
      <div>
        <div class="label">需要令牌</div>
        <div>输入服务器 UPLOAD_TOKEN 后保存，后台会自动刷新。令牌只保存在当前浏览器。</div>
      </div>
      <button id="focusToken">输入 token</button>
    </div>

    <div id="dashboardBody" class="hidden">
      <div class="grid">
        <div class="panel"><div class="label">服务</div><div id="service" class="value">-</div></div>
        <div class="panel"><div class="label">任务总数</div><div id="total" class="value">-</div></div>
        <div class="panel"><div class="label">正在上传</div><div id="active" class="value">-</div></div>
        <div class="panel"><div class="label">失败任务</div><div id="failed" class="value">-</div></div>
      </div>

      <section class="panel">
        <div class="section-title"><h2>设备 / Wi-Fi</h2><span class="muted">来自小机器最近一次上传</span></div>
        <table><thead><tr><th>设备</th><th>最后状态</th><th>Wi-Fi</th><th>最近录音</th><th>更新时间</th></tr></thead><tbody id="devices"></tbody></table>
      </section>

      <section class="panel">
        <div class="section-title"><h2>正在上传</h2><span class="muted">3 秒自动刷新</span></div>
        <table><thead><tr><th>录音</th><th>设备</th><th>格式</th><th>进度</th><th>字节</th><th>速度</th><th>剩余</th><th>开始时间</th></tr></thead><tbody id="uploads"></tbody></table>
      </section>

      <section class="panel">
        <div class="section-title">
          <h2>最近任务</h2>
          <div class="filters">
            <button type="button" class="active" data-status="">全部</button>
            <button type="button" data-status="failed">失败</button>
            <button type="button" data-status="done">完成</button>
            <button type="button" data-status="uploaded">已上传</button>
            <button type="button" data-status="transcribing">转写中</button>
          </div>
        </div>
        <div class="muted" style="margin-bottom:8px">失败任务可重跑，重发 flomo 会产生新 memo</div>
        <table><thead><tr><th>录音</th><th>状态</th><th>设备</th><th>大小</th><th>记录时间</th><th>更新</th><th>备注</th><th>操作</th></tr></thead><tbody id="jobs"></tbody></table>
      </section>
    </div>
  </main>
  <script>
    const $ = (id) => document.getElementById(id);
    const fmtTime = (v) => v ? new Date(v).toLocaleString() : '-';
    const fmtBytes = (n) => Number.isFinite(n) ? (n > 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' KB') : '-';
    const fmtRate = (n) => Number.isFinite(n) && n > 0 ? (n > 1048576 ? (n / 1048576).toFixed(2) + ' MB/s' : Math.round(n / 1024) + ' KB/s') : '-';
    const fmtDuration = (seconds) => {
      if (!Number.isFinite(seconds) || seconds <= 0) return '-';
      const s = Math.round(seconds);
      const m = Math.floor(s / 60);
      const r = s % 60;
      return m ? m + 'm ' + r + 's' : r + 's';
    };
    const esc = (v) => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const normalizeToken = (value) => String(value || '').trim().replace(/^UPLOAD_TOKEN\\s*=\\s*/i, '').replace(/^token\\s*=\\s*/i, '').replace(/^['"]|['"]$/g, '').trim();
    const tokenInput = $('token');
    let statusFilter = '';
    tokenInput.value = localStorage.getItem('cardputerUploadToken') || '';
    $('save').onclick = () => { tokenInput.value = normalizeToken(tokenInput.value); localStorage.setItem('cardputerUploadToken', tokenInput.value); load(); };
    $('refresh').onclick = () => load();
    $('clear').onclick = () => { localStorage.removeItem('cardputerUploadToken'); tokenInput.value = ''; load(); tokenInput.focus(); };
    $('focusToken').onclick = () => tokenInput.focus();
    $('limit').onchange = () => load();
    document.querySelectorAll('[data-status]').forEach((button) => {
      button.onclick = () => {
        statusFilter = button.dataset.status || '';
        document.querySelectorAll('[data-status]').forEach((item) => item.classList.toggle('active', item === button));
        load();
      };
    });

    function statusClass(status) {
      if (status === 'done' || status === 'uploaded' || status === 'transcribed') return 'ok';
      if (String(status || '').includes('failed')) return 'bad';
      return 'warn';
    }

    function statusPill(status) {
      return '<span class="pill ' + statusClass(status) + '">' + esc(status || '-') + '</span>';
    }

    async function load() {
      const token = normalizeToken(tokenInput.value);
      if (tokenInput.value && tokenInput.value !== token) tokenInput.value = token;
      $('error').textContent = '';
      $('loginHint').classList.toggle('hidden', Boolean(token));
      $('dashboardBody').classList.toggle('hidden', !token);
      $('stamp').textContent = token ? '正在读取...' : '等待登录';
      if (!token) return;
      try {
        const params = new URLSearchParams({ limit: $('limit').value });
        if (statusFilter) params.set('status', statusFilter);
        const res = await fetch('/api/dashboard?' + params.toString(), {
          headers: { 'X-Upload-Token': token }
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        render(data);
      } catch (error) {
        $('loginHint').classList.remove('hidden');
        $('dashboardBody').classList.add('hidden');
        $('error').textContent = error.message === 'invalid upload token'
          ? 'token 不正确：请只粘贴等号后面的 UPLOAD_TOKEN'
          : error.message;
        $('stamp').textContent = '读取失败';
      }
    }

    async function postJob(id, action) {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      const isResend = action === 'resend';
      if (isResend && !confirm('确认重新发送 ' + id + ' 到 flomo？这会产生一条新的 memo。')) return;
      try {
        const res = await fetch('/jobs/' + encodeURIComponent(id) + '/' + action, {
          method: 'POST',
          headers: { 'X-Upload-Token': token }
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        $('error').textContent = '';
        $('stamp').textContent = id + ' 已提交 ' + action;
        setTimeout(load, 500);
      } catch (error) {
        $('error').textContent = error.message;
      }
    }

    window.reprocessJob = (id) => postJob(id, 'process');
    window.resendJob = (id) => postJob(id, 'resend');

    function render(data) {
      $('service').innerHTML = data.health.ok ? '<span class="ok">OK</span>' : '<span class="bad">ERR</span>';
      const statusCounts = data.jobs.statusCounts || {};
      const failedTotal = Object.keys(statusCounts).reduce((sum, key) => key.includes('failed') ? sum + statusCounts[key] : sum, 0);
      $('total').textContent = data.jobs.totalAll ?? data.jobs.total;
      $('active').textContent = data.activeUploads.length;
      $('failed').textContent = failedTotal;
      $('stamp').textContent = '更新 ' + fmtTime(data.time);

      $('devices').innerHTML = data.devices.length ? data.devices.map(d => {
        const wifi = d.wifiRssi === undefined ? '<span class="muted">未上报</span>' : 'RSSI ' + esc(d.wifiRssi) + ' / IP ' + esc(d.wifiIp || '-');
        return '<tr><td>' + esc(d.deviceId) + '</td><td>' + statusPill(d.lastStatus) + '</td><td>' + wifi + '</td><td>' + esc(d.lastRecordingName || '-') + '</td><td class="nowrap">' + fmtTime(d.updatedAt) + '</td></tr>';
      }).join('') : '<tr><td colspan="5" class="muted">还没有设备上报。下一次上传开始后会出现。</td></tr>';

      $('uploads').innerHTML = data.activeUploads.length ? data.activeUploads.map(u => {
        const pct = u.totalBytes ? Math.min(100, Math.round(u.bytesReceived * 100 / u.totalBytes)) : 0;
        const elapsed = Math.max(0.001, (Date.now() - Date.parse(u.startedAt || Date.now())) / 1000);
        const rate = u.bytesReceived / elapsed;
        const eta = u.totalBytes && rate > 0 ? (u.totalBytes - u.bytesReceived) / rate : NaN;
        const encoding = u.encoding === 'ima-adpcm' ? 'ADPCM' : 'WAV';
        const bytesLabel = (u.originalBytes && u.originalBytes > u.totalBytes)
          ? fmtBytes(u.bytesReceived) + ' / ' + fmtBytes(u.totalBytes) + '<br><span class="muted">原始 ' + fmtBytes(u.originalBytes) + '</span>'
          : fmtBytes(u.bytesReceived) + ' / ' + fmtBytes(u.totalBytes);
        return '<tr><td>' + esc(u.recordingName) + '</td><td>' + esc(u.deviceId) + '</td><td>' + encoding + '</td><td><div class="progress"><div class="fill" style="width:' + pct + '%"></div></div> ' + pct + '%</td><td>' + bytesLabel + '</td><td class="nowrap">' + fmtRate(rate) + '</td><td class="nowrap">' + fmtDuration(eta) + '</td><td>' + fmtTime(u.startedAt) + '</td></tr>';
      }).join('') : '<tr><td colspan="8" class="muted">当前没有正在接收的上传。</td></tr>';

      $('jobs').innerHTML = data.jobs.jobs.length ? data.jobs.jobs.map(j => {
        const note = j.lastError || j.pendingReason || (j.memo && j.memo.title) || '';
        const canProcess = String(j.status || '').includes('failed') || j.status === 'uploaded' || j.status === 'transcribed';
        const canResend = j.status === 'done' || j.status === 'transcribed';
        const actions = '<div class="actions">' +
          '<a class="button" href="/dashboard/jobs/' + encodeURIComponent(j.id) + '">查看</a>' +
          (canProcess ? '<button class="small" onclick="reprocessJob(\\'' + esc(j.id) + '\\')">重跑</button>' : '') +
          (canResend ? '<button class="small danger" onclick="resendJob(\\'' + esc(j.id) + '\\')">重发</button>' : '') +
          '</div>';
        const sizeLabel = j.uploadEncoding === 'ima-adpcm' && j.uploadedBytes
          ? fmtBytes(j.bytes) + '<br><span class="muted">上传 ' + fmtBytes(j.uploadedBytes) + ' / ' + esc(j.compressionRatio || '-') + 'x</span>'
          : fmtBytes(j.bytes);
        return '<tr><td class="nowrap">' + esc(j.id) + '</td><td>' + statusPill(j.status) + '</td><td>' + esc(j.deviceId || '-') + '</td><td class="nowrap">' + sizeLabel + '</td><td>' + esc(j.recordedAt || '-') + '</td><td class="nowrap">' + fmtTime(j.updatedAt || j.createdAt) + '</td><td>' + esc(note) + '</td><td>' + actions + '</td></tr>';
      }).join('') : '<tr><td colspan="8" class="muted">没有匹配任务。</td></tr>';
    }

    load();
    setInterval(load, 3000);
  </script>
</body>
</html>`;
}

function dashboardLabHtml() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cardputer Dashboard</title>
  <style>
    :root { color-scheme: dark; --bg:#070b09; --panel:#101613; --soft:#0c120f; --line:#21412f; --text:#e8fff0; --muted:#8fb09b; --ok:#40ff83; --bad:#ff5d5d; --warn:#ffd166; }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }
    header { border-bottom: 1px solid var(--line); background: #090f0c; position: sticky; top: 0; z-index: 5; }
    .head, main { max-width: 1380px; margin: 0 auto; padding: 14px 18px; }
    h1 { margin: 0 0 10px; font-size: 22px; }
    h2 { margin: 0 0 10px; font-size: 15px; color: var(--ok); }
    input, button, select, textarea { border: 1px solid var(--line); background: var(--soft); color: var(--text); border-radius: 6px; padding: 0 10px; font: inherit; min-width: 0; }
    input, button, select { height: 38px; }
    textarea { width: 100%; min-height: 80px; padding: 10px; resize: vertical; }
    button { cursor: pointer; color: var(--ok); white-space: nowrap; }
    button:hover { border-color: var(--ok); }
    .bar { display: grid; grid-template-columns: minmax(220px, 1fr) auto auto auto auto; gap: 8px; align-items: center; }
    .layout { display: grid; grid-template-columns: minmax(280px, 380px) minmax(0, 1fr); gap: 14px; align-items: start; }
    .panel { border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 12px; }
    .stack { display: grid; gap: 14px; }
    .list { display: grid; gap: 8px; max-height: calc(100vh - 190px); overflow: auto; padding-right: 4px; }
    .job { border: 1px solid #17251d; background: #080d0a; border-radius: 8px; padding: 10px; cursor: pointer; }
    .job:hover, .job.active { border-color: var(--ok); background: #0d1711; }
    .job-title { display: flex; justify-content: space-between; gap: 8px; align-items: center; font-weight: 700; }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }
    .grid.two { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .label { color: var(--muted); font-size: 12px; margin-bottom: 6px; }
    .param { border: 1px solid #17251d; background: #080d0a; border-radius: 8px; padding: 10px; min-width: 0; }
    .param input { width: 100%; }
    .param-hint { color: var(--muted); font-size: 12px; line-height: 1.35; margin-top: 6px; }
    .tune-note { border: 1px solid #243925; background: #0b130e; border-radius: 8px; padding: 10px; color: var(--muted); font-size: 13px; line-height: 1.45; margin-bottom: 10px; }
    .value { font-size: 16px; font-weight: 700; overflow-wrap: anywhere; }
    .muted { color: var(--muted); }
    .error, .bad { color: var(--bad); }
    .ok { color: var(--ok); }
    .warn { color: var(--warn); }
    .pill { display: inline-block; border: 1px solid var(--line); border-radius: 999px; padding: 2px 7px; white-space: nowrap; font-size: 12px; }
    .pill.ok { color: var(--ok); }
    .pill.bad { color: var(--bad); }
    .pill.warn { color: var(--warn); }
    .section-title { display: flex; justify-content: space-between; gap: 10px; align-items: center; margin-bottom: 10px; }
    .section-title h2 { margin: 0; }
    .statusline, .actions { display: flex; gap: 8px; align-items: center; min-height: 24px; margin-top: 8px; color: var(--muted); font-size: 13px; flex-wrap: wrap; }
    .kv { display: grid; grid-template-columns: 120px minmax(0, 1fr); gap: 8px 12px; font-size: 14px; }
    .kv div:nth-child(odd) { color: var(--muted); }
    audio { width: 100%; margin-top: 8px; }
    pre { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font: 12px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace; background: #080d0a; border: 1px solid #17251d; border-radius: 6px; padding: 10px; max-height: 230px; overflow: auto; }
    .feedback-item { border: 1px solid #17251d; border-radius: 6px; padding: 10px; background: #080d0a; margin-top: 8px; }
    .hidden { display: none !important; }
    a { color: var(--ok); }
    @media (max-width: 980px) { .layout { grid-template-columns: 1fr; } .list { max-height: 360px; } .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .bar { grid-template-columns: 1fr auto auto; } }
    @media (max-width: 560px) { .grid, .grid.two, .kv { grid-template-columns: 1fr; } .bar { grid-template-columns: 1fr 1fr; } #token { grid-column: 1 / -1; } }
  </style>
</head>
<body>
  <header><div class="head">
    <div class="section-title"><h1>Cardputer Dashboard</h1><span class="muted">一个页面完成查看、试听、调参和重跑</span></div>
    <div class="bar">
      <input id="token" type="password" autocomplete="off" placeholder="UPLOAD_TOKEN">
      <button id="save">保存</button><button id="refresh">刷新列表</button><button id="clear">清除</button>
      <select id="limit"><option>20</option><option selected>50</option><option>100</option></select>
    </div>
    <div class="statusline"><span id="stamp">等待登录</span><span id="error" class="error"></span></div>
  </div></header>
  <main>
    <div class="layout">
      <aside class="panel">
        <div class="section-title"><h2>录音列表</h2><span id="listCount" class="muted">-</span></div>
        <div class="actions"><select id="statusFilter"><option value="">全部</option><option value="done">完成</option><option value="failed">失败</option><option value="uploaded">已上传</option></select><button id="reloadJobs">刷新</button></div>
        <div id="jobs" class="list"><div class="muted">输入 token 后读取。</div></div>
      </aside>
      <section class="stack">
        <div class="panel">
          <div class="section-title"><h2 id="currentTitle">选择一条录音</h2><span id="currentStatus" class="muted"></span></div>
          <div class="grid">
            <div><div class="label">上传格式</div><div id="encoding" class="value">-</div></div>
            <div><div class="label">大小</div><div id="bytes" class="value">-</div></div>
            <div><div class="label">时长</div><div id="duration" class="value">-</div></div>
            <div><div class="label">压缩倍率</div><div id="ratio" class="value">-</div></div>
          </div>
          <div class="statusline"><span id="jobMeta"></span></div>
        </div>
        <div id="labBody" class="hidden stack">
          <div class="panel">
            <div class="section-title"><h2>试听对比</h2><span id="audioStatus" class="muted"></span></div>
            <div class="grid">
              <div><div class="label">原始录音</div><audio id="rawAudio" controls></audio></div>
              <div><div class="label">人耳试听版</div><audio id="previewAudio" controls></audio></div>
              <div><div class="label">ASR 识别版</div><audio id="asrAudio" controls></audio></div>
              <div><div class="label">快速操作</div><div class="actions"><button id="loadAllAudio">加载全部</button><button id="pauseAudio">暂停</button></div></div>
            </div>
            <div class="actions"><button id="playRawStart">听原始</button><button id="playPreviewStart">听试听</button><button id="playAsrStart">听识别</button><button id="switchToPreview">同位置切试听</button><button id="switchToAsr">同位置切识别</button><button id="switchToRaw">同位置切原始</button></div>
          </div>
          <div class="panel">
            <div class="section-title"><h2>试听参数</h2><span id="previewStatus" class="muted"></span></div>
            <div class="tune-note">建议顺序：先用“默认”生成一次；如果一卡一卡，把“底噪保留比例”调大，或把“底噪 RMS 上限”调小；如果刮擦刺耳，再降低“高频保留”。阈值类参数最后再动。</div>
            <div class="grid">
              <div class="param"><div class="label">增益</div><input id="previewGain" type="number" step="0.1" min="0.2" max="3"><div class="param-hint">整体音量。大了更响，也更容易把底噪一起放大。</div></div>
              <div class="param"><div class="label">低通系数</div><input id="previewLowpass" type="number" step="1" min="1" max="255"><div class="param-hint">刮擦命中时怎么削尖声。小=更柔但更闷；大=更清楚但刺声更多。</div></div>
              <div class="param"><div class="label">高频保留</div><input id="previewHighMix" type="number" step="0.05" min="0" max="1"><div class="param-hint">刮擦命中时保留多少亮度。大=自然清晰；小=更降刺但人声会闷。</div></div>
              <div class="param"><div class="label">命中保持帧</div><input id="previewHold" type="number" step="1" min="0" max="20"><div class="param-hint">命中刮擦后延续处理多久。大=更稳；太大会拖尾、发闷。</div></div>
              <div class="param"><div class="label">RMS 上限</div><input id="previewRms" type="number" step="50" min="0" max="10000"><div class="param-hint">只处理低音量刮擦。大=更容易命中；太大会误伤轻声人声。</div></div>
              <div class="param"><div class="label">Diff 下限</div><input id="previewDiff" type="number" step="10" min="0" max="5000"><div class="param-hint">高频变化多大才算刮擦。小=更敏感；大=只抓明显刮擦。</div></div>
              <div class="param"><div class="label">Diff/RMS 比例</div><input id="previewRatio" type="number" step="5" min="1" max="512"><div class="param-hint">刮擦“尖锐程度”门槛。小=更容易处理；大=更保守。</div></div>
              <div class="param"><div class="label">帧采样数</div><input id="previewFrame" type="number" step="64" min="64" max="2048"><div class="param-hint">分析窗口大小。小=反应快但可能抖；大=更平滑但反应慢。</div></div>
              <div class="param"><div class="label">底噪 RMS 上限</div><input id="previewNoiseRms" type="number" step="25" min="0" max="5000"><div class="param-hint">低于多少算底噪。大=降噪更多；太大会把轻声压没。</div></div>
              <div class="param"><div class="label">底噪保留比例</div><input id="previewNoiseMix" type="number" step="0.05" min="0" max="1"><div class="param-hint">底噪保留多少。大=自然不卡；小=更安静但容易一抽一抽。</div></div>
            </div>
            <div class="actions"><button id="presetGentle">轻柔</button><button id="presetDefault">默认</button><button id="presetStrong">强力</button><button id="generatePreview">生成并加载试听版</button><button id="loadPreview">只加载试听版</button></div>
            <div id="previewMeta" class="kv" style="margin-top:10px"></div>
          </div>
          <div class="panel">
            <div class="section-title"><h2>ASR 识别参数</h2><span id="asrStatus" class="muted"></span></div>
            <div class="tune-note">这一组主要给转写用，不一定好听。一般只需要动“增益”和“噪声门 RMS”；如果转写漏字，先把噪声门 RMS 调低或门限保留调高。</div>
            <div class="grid">
              <div class="param"><div class="label">增益</div><input id="asrGain" type="number" step="0.05" min="0.2" max="4"><div class="param-hint">给转写版提音量。大了更容易识别轻声，也会放大底噪。</div></div>
              <div class="param"><div class="label">高通系数</div><input id="asrHighpass" type="number" step="0.001" min="0" max="0.999"><div class="param-hint">削低频轰隆声。接近 1 更保留低频；小一点削得更多。</div></div>
              <div class="param"><div class="label">预加重</div><input id="asrPreEmphasis" type="number" step="0.05" min="0" max="0.95"><div class="param-hint">突出字头和清晰度。大了更清楚，也可能更尖。</div></div>
              <div class="param"><div class="label">噪声门 RMS</div><input id="asrNoiseGateRms" type="number" step="20" min="0" max="5000"><div class="param-hint">低于多少当噪声压下去。大=更干净；太大会漏掉轻声。</div></div>
              <div class="param"><div class="label">门限保留</div><input id="asrNoiseGateFloor" type="number" step="0.05" min="0" max="1"><div class="param-hint">被判噪声时还保留多少。大=不容易断字；小=更安静。</div></div>
              <div class="param"><div class="label">压缩阈值</div><input id="asrCompressorThreshold" type="number" step="100" min="500" max="24000"><div class="param-hint">多大声音开始压缩。小=大小声更平均；太小会闷。</div></div>
              <div class="param"><div class="label">压缩比</div><input id="asrCompressorRatio" type="number" step="0.1" min="1" max="12"><div class="param-hint">压缩强度。大=大声被压更多；太大会不自然。</div></div>
              <div class="param"><div class="label">目标峰值</div><input id="asrTargetPeak" type="number" step="500" min="4000" max="32000"><div class="param-hint">处理后目标音量峰值。大=更响；太大容易刺耳。</div></div>
              <div class="param"><div class="label">限幅</div><input id="asrLimiter" type="number" step="500" min="4000" max="32767"><div class="param-hint">最高不超过多少，防爆音。一般不用动。</div></div>
              <div class="param"><div class="label">帧采样数</div><input id="asrFrameSamples" type="number" step="80" min="80" max="2048"><div class="param-hint">噪声判断窗口。小=反应快；大=更平滑。</div></div>
            </div>
            <div class="actions"><button id="asrPresetDefault">ASR 默认</button><button id="generateAsrClean">生成并加载识别版</button><button id="loadAsrClean">只加载识别版</button><button id="reprocessFromAsr">用识别版重跑转写</button></div>
            <div id="asrMeta" class="kv" style="margin-top:10px"></div>
          </div>
          <div class="panel">
            <div class="section-title"><h2>听感记录</h2><span id="feedbackStatus" class="muted"></span></div>
            <div class="grid two"><div><div class="label">结论</div><select id="feedbackRating"><option value="">选择结论</option><option value="better">更好</option><option value="worse">更差</option><option value="mixed">有好有坏</option><option value="neutral">差不多</option></select></div><div><div class="label">保存</div><button id="saveFeedback">保存听感记录</button></div></div>
            <div style="margin-top:10px"><div class="label">备注</div><textarea id="feedbackNote" placeholder="例如：默认版刮擦少了，但人声闷；轻柔版更自然。"></textarea></div>
            <div id="feedbackList"></div>
          </div>
          <div class="panel">
            <div class="section-title"><h2>转写 / memo</h2><span class="muted">辅助判断</span></div>
            <div class="grid two"><div><div class="label">转写</div><pre id="transcript">-</pre></div><div><div class="label">flomo memo</div><pre id="memo">-</pre></div></div>
          </div>
        </div>
      </section>
    </div>
  </main>
  <script>
    const $ = (id) => document.getElementById(id);
    const esc = (v) => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const fmtTime = (v) => v ? new Date(v).toLocaleString() : '-';
    const fmtBytes = (n) => Number.isFinite(n) ? (n > 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' KB') : '-';
    const fmtDuration = (s) => Number.isFinite(s) ? (s >= 60 ? Math.floor(s / 60) + 'm ' + Math.round(s % 60) + 's' : s.toFixed(1) + 's') : '-';
    const normalizeToken = (value) => String(value || '').trim().replace(/^UPLOAD_TOKEN\\s*=\\s*/i, '').replace(/^token\\s*=\\s*/i, '').replace(/^['"]|['"]$/g, '').trim();
    const tokenInput = $('token');
    let jobs = [];
    let currentId = '';
    let selectionSeq = 0;
    const initialJobId = new URLSearchParams(location.search).get('job') || '';
    const defaultPreviewParams = { gain: 1, lowpass: 48, highMix: 0.72, scratchRmsMax: 1200, scratchDiffMin: 260, scratchRatio: 145, holdFrames: 0, frameSamples: 256, noiseRmsMax: 420, noiseMix: 0.78 };
    const defaultAsrParams = { gain: 1.25, highpass: 0.985, preEmphasis: 0.18, noiseGateRms: 520, noiseGateFloor: 0.18, compressorThreshold: 5200, compressorRatio: 3.2, targetPeak: 26000, limiter: 30000, frameSamples: 320 };
    tokenInput.value = localStorage.getItem('cardputerUploadToken') || '';
    $('save').onclick = () => { tokenInput.value = normalizeToken(tokenInput.value); localStorage.setItem('cardputerUploadToken', tokenInput.value); loadJobs(); };
    $('refresh').onclick = () => loadJobs();
    $('reloadJobs').onclick = () => loadJobs();
    $('statusFilter').onchange = () => loadJobs();
    $('limit').onchange = () => loadJobs();
    $('clear').onclick = () => { localStorage.removeItem('cardputerUploadToken'); tokenInput.value = ''; jobs = []; renderJobs(); };
    $('presetGentle').onclick = () => setPreviewParams({ gain: 1, lowpass: 36, highMix: 0.86, scratchRmsMax: 900, scratchDiffMin: 320, scratchRatio: 170, holdFrames: 0, frameSamples: 256, noiseRmsMax: 260, noiseMix: 0.9 });
    $('presetDefault').onclick = () => setPreviewParams(defaultPreviewParams);
    $('presetStrong').onclick = () => setPreviewParams({ gain: 1, lowpass: 72, highMix: 0.55, scratchRmsMax: 1700, scratchDiffMin: 190, scratchRatio: 115, holdFrames: 1, frameSamples: 256, noiseRmsMax: 760, noiseMix: 0.62 });
    $('generatePreview').onclick = () => generatePreview();
    $('loadPreview').onclick = () => loadPreviewAudio();
    $('asrPresetDefault').onclick = () => setAsrParams(defaultAsrParams);
    $('generateAsrClean').onclick = () => generateAsrClean();
    $('loadAsrClean').onclick = () => loadAsrCleanAudio();
    $('reprocessFromAsr').onclick = () => postJob('process');
    $('loadAllAudio').onclick = () => loadAllAudio();
    $('saveFeedback').onclick = () => saveFeedback();
    $('playRawStart').onclick = () => playOnly($('rawAudio'), 0);
    $('playPreviewStart').onclick = () => playOnly($('previewAudio'), 0);
    $('playAsrStart').onclick = () => playOnly($('asrAudio'), 0);
    $('switchToPreview').onclick = () => playOnly($('previewAudio'), $('rawAudio').currentTime || $('previewAudio').currentTime || 0);
    $('switchToAsr').onclick = () => playOnly($('asrAudio'), $('rawAudio').currentTime || $('previewAudio').currentTime || $('asrAudio').currentTime || 0);
    $('switchToRaw').onclick = () => playOnly($('rawAudio'), $('previewAudio').currentTime || $('rawAudio').currentTime || 0);
    $('pauseAudio').onclick = () => pauseBoth();
    setPreviewParams(defaultPreviewParams);
    setAsrParams(defaultAsrParams);
    function token() { const value = normalizeToken(tokenInput.value); if (tokenInput.value && tokenInput.value !== value) tokenInput.value = value; return value; }
    function statusClass(status) { if (status === 'done' || status === 'uploaded' || status === 'transcribed') return 'ok'; if (String(status || '').includes('failed')) return 'bad'; return 'warn'; }
    function statusPill(status) { return '<span class="pill ' + statusClass(status) + '">' + esc(status || '-') + '</span>'; }
    function setPreviewParams(params) { const p = { ...defaultPreviewParams, ...(params || {}) }; $('previewGain').value = p.gain; $('previewLowpass').value = p.lowpass; $('previewHighMix').value = p.highMix; $('previewHold').value = p.holdFrames; $('previewRms').value = p.scratchRmsMax; $('previewDiff').value = p.scratchDiffMin; $('previewRatio').value = p.scratchRatio; $('previewFrame').value = p.frameSamples; $('previewNoiseRms').value = p.noiseRmsMax; $('previewNoiseMix').value = p.noiseMix; }
    function previewParamsFromForm() { return { gain: Number($('previewGain').value), lowpass: Number($('previewLowpass').value), highMix: Number($('previewHighMix').value), holdFrames: Number($('previewHold').value), scratchRmsMax: Number($('previewRms').value), scratchDiffMin: Number($('previewDiff').value), scratchRatio: Number($('previewRatio').value), frameSamples: Number($('previewFrame').value), noiseRmsMax: Number($('previewNoiseRms').value), noiseMix: Number($('previewNoiseMix').value) }; }
    function setAsrParams(params) { const p = { ...defaultAsrParams, ...(params || {}) }; $('asrGain').value = p.gain; $('asrHighpass').value = p.highpass; $('asrPreEmphasis').value = p.preEmphasis; $('asrNoiseGateRms').value = p.noiseGateRms; $('asrNoiseGateFloor').value = p.noiseGateFloor; $('asrCompressorThreshold').value = p.compressorThreshold; $('asrCompressorRatio').value = p.compressorRatio; $('asrTargetPeak').value = p.targetPeak; $('asrLimiter').value = p.limiter; $('asrFrameSamples').value = p.frameSamples; }
    function asrParamsFromForm() { return { gain: Number($('asrGain').value), highpass: Number($('asrHighpass').value), preEmphasis: Number($('asrPreEmphasis').value), noiseGateRms: Number($('asrNoiseGateRms').value), noiseGateFloor: Number($('asrNoiseGateFloor').value), compressorThreshold: Number($('asrCompressorThreshold').value), compressorRatio: Number($('asrCompressorRatio').value), targetPeak: Number($('asrTargetPeak').value), limiter: Number($('asrLimiter').value), frameSamples: Number($('asrFrameSamples').value) }; }
    function kv(rows) { return rows.map(([k, v]) => '<div>' + esc(k) + '</div><div>' + (v || '-') + '</div>').join(''); }
    function ratingLabel(value) { return ({ better: '更好', worse: '更差', mixed: '有好有坏', neutral: '差不多' })[value] || value || '-'; }
    function renderPreviewMeta(preview) { if (!preview) { $('previewMeta').innerHTML = ''; return; } $('previewMeta').innerHTML = kv([['处理帧', esc((preview.metrics?.processedFrames ?? '-') + ' / ' + (preview.metrics?.totalFrames ?? '-'))], ['摩擦命中帧', esc(preview.metrics?.detectedFrames ?? '-')], ['底噪处理帧', esc(preview.metrics?.noiseFrames ?? '-')], ['时长', fmtDuration(preview.metrics?.durationSec)], ['生成时间', fmtTime(preview.createdAt)]]); }
    function renderAsrMeta(asrClean) { if (!asrClean) { $('asrMeta').innerHTML = ''; return; } $('asrMeta').innerHTML = kv([['门限处理帧', esc((asrClean.metrics?.gatedFrames ?? '-') + ' / ' + (asrClean.metrics?.totalFrames ?? '-'))], ['压缩样本', esc(asrClean.metrics?.compressedSamples ?? '-')], ['限幅样本', esc(asrClean.metrics?.limitedSamples ?? '-')], ['峰值变化', esc((asrClean.metrics?.inputPeak ?? '-') + ' → ' + (asrClean.metrics?.outputPeak ?? '-'))], ['归一化增益', esc(asrClean.metrics?.normalizeGain ?? '-')], ['时长', fmtDuration(asrClean.metrics?.durationSec)], ['生成时间', fmtTime(asrClean.createdAt)]]); }
    function renderFeedback(items) { const list = Array.isArray(items) ? items : []; $('feedbackList').innerHTML = list.length ? list.map((item) => { const metrics = item.metrics ? '处理 ' + (item.metrics.processedFrames ?? '-') + '/' + (item.metrics.totalFrames ?? '-') + '，命中 ' + (item.metrics.detectedFrames ?? '-') : ''; return '<div class="feedback-item"><strong>' + esc(ratingLabel(item.rating)) + '</strong><span class="muted"> · ' + esc(fmtTime(item.createdAt)) + '</span><div>' + esc(item.note || '-') + '</div><div class="muted">' + esc(metrics) + '</div></div>'; }).join('') : '<div class="muted">还没有听感记录。</div>'; }
    function renderJobs() { $('listCount').textContent = jobs.length ? jobs.length + ' 条' : '-'; $('jobs').innerHTML = jobs.length ? jobs.map((job) => { const encoding = job.uploadEncoding === 'ima-adpcm' ? 'ADPCM' : 'WAV'; return '<div class="job ' + (job.id === currentId ? 'active' : '') + '" data-id="' + esc(job.id) + '"><div class="job-title"><span>' + esc(job.id) + '</span>' + statusPill(job.status) + '</div><div class="muted">' + esc(job.recordingName || '-') + ' · ' + encoding + ' · ' + fmtBytes(job.bytes) + '</div><div class="muted">' + esc(job.recordedAt || '-') + '</div></div>'; }).join('') : '<div class="muted">没有任务。</div>'; document.querySelectorAll('.job[data-id]').forEach((el) => { el.onclick = () => selectJob(el.dataset.id); }); }
    function pauseBoth() { $('rawAudio').pause(); $('previewAudio').pause(); $('asrAudio').pause(); }
    async function playOnly(audio, at) { pauseBoth(); if (!audio.src) return; const time = Number.isFinite(at) ? Math.max(0, at) : 0; if (Number.isFinite(audio.duration) && audio.duration > 0) audio.currentTime = Math.min(time, Math.max(0, audio.duration - 0.05)); else audio.currentTime = time; await audio.play().catch(() => {}); }
    async function loadJobs() { const uploadToken = token(); if (!uploadToken) { $('stamp').textContent = '等待登录'; return; } $('stamp').textContent = '正在读取列表...'; $('error').textContent = ''; try { const params = new URLSearchParams({ limit: $('limit').value }); const status = $('statusFilter').value; if (status) params.set('status', status); const res = await fetch('/api/dashboard?' + params.toString(), { headers: { 'X-Upload-Token': uploadToken } }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); jobs = data.jobs.jobs || []; $('stamp').textContent = '更新 ' + fmtTime(data.time); renderJobs(); if (!currentId) { const first = jobs.find((job) => job.id === initialJobId) || jobs[0]; if (first) await selectJob(first.id); } } catch (error) { $('error').textContent = error.message === 'invalid upload token' ? 'token 不正确' : error.message; } }
    function revokeAudio(audio) { audio.pause(); if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url); audio.dataset.url = ''; audio.removeAttribute('src'); audio.load(); }
    function clearPlayers() { revokeAudio($('rawAudio')); revokeAudio($('previewAudio')); revokeAudio($('asrAudio')); $('audioStatus').textContent = '未加载'; $('previewStatus').textContent = '未加载'; $('asrStatus').textContent = '未加载'; }
    async function selectJob(id) { const seq = ++selectionSeq; currentId = id; renderJobs(); $('labBody').classList.remove('hidden'); $('currentTitle').textContent = id; $('currentStatus').textContent = '正在读取...'; clearPlayers(); history.replaceState(null, '', '/dashboard?job=' + encodeURIComponent(id)); try { const res = await fetch('/api/jobs/' + encodeURIComponent(id), { headers: { 'X-Upload-Token': token() } }); const data = await res.json(); if (seq !== selectionSeq || id !== currentId) return; if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); renderJob(data); await loadRawAudio(id, seq); if (data.preview?.metrics?.algorithm === 'smooth-play-preview-v2' && data.files?.preview) await loadPreviewAudio(false, id, seq); if (data.files?.asrClean) await loadAsrCleanAudio(false, id, seq); } catch (error) { if (seq === selectionSeq) $('currentStatus').textContent = error.message; } }
    function renderJob(data) { const job = data.job; const wav = data.files?.audio?.wav || {}; const encoding = job.uploadEncoding === 'ima-adpcm' ? 'ADPCM' : 'WAV'; $('currentTitle').textContent = job.id; $('currentStatus').innerHTML = statusPill(job.status); $('encoding').textContent = encoding; $('bytes').textContent = fmtBytes(job.bytes); $('duration').textContent = fmtDuration(wav.durationSec); $('ratio').textContent = job.compressionRatio ? job.compressionRatio + 'x' : '-'; $('jobMeta').textContent = (job.recordedAt || '-') + ' · ' + (job.deviceId || '-'); $('transcript').textContent = data.transcriptText || '还没有转写文本。'; $('memo').textContent = data.memoText || '还没有 flomo memo。'; if (data.preview?.metrics?.algorithm === 'smooth-play-preview-v2' && data.preview?.params) { setPreviewParams(data.preview.params); renderPreviewMeta(data.preview); $('previewStatus').textContent = '已读取 v2 试听参数。'; } else { setPreviewParams(defaultPreviewParams); renderPreviewMeta(null); $('previewStatus').textContent = data.files?.preview ? '旧试听版已忽略，请点“生成并加载试听版”。' : '还没有试听版。'; } if (data.asrClean?.params) { setAsrParams(data.asrClean.params); renderAsrMeta(data.asrClean); $('asrStatus').textContent = '已读取上次 ASR 参数。'; } else { setAsrParams(defaultAsrParams); renderAsrMeta(null); $('asrStatus').textContent = '还没有识别版。'; } renderFeedback(data.previewFeedback); }
    async function loadAudioTo(url, audio, statusEl, successText, id = currentId, seq = selectionSeq) { const res = await fetch(url, { headers: { 'X-Upload-Token': token() } }); if (!res.ok) { const data = await res.json().catch(() => ({})); throw new Error(data.error || 'HTTP ' + res.status); } const blob = await res.blob(); if (seq !== selectionSeq || id !== currentId) return; revokeAudio(audio); const objectUrl = URL.createObjectURL(blob); audio.dataset.url = objectUrl; audio.src = objectUrl; audio.load(); if (statusEl) statusEl.textContent = successText; }
    async function loadRawAudio(id = currentId, seq = selectionSeq) { $('audioStatus').textContent = '正在加载原始录音...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/audio?t=' + Date.now(), $('rawAudio'), $('audioStatus'), '原始录音已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('audioStatus').textContent = error.message; } }
    async function loadPreviewAudio(showStatus = true, id = currentId, seq = selectionSeq) { if (showStatus) $('previewStatus').textContent = '正在加载试听版...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/preview/audio?t=' + Date.now(), $('previewAudio'), $('previewStatus'), '试听版已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('previewStatus').textContent = error.message; } }
    async function loadAsrCleanAudio(showStatus = true, id = currentId, seq = selectionSeq) { if (showStatus) $('asrStatus').textContent = '正在加载识别版...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/asr-clean/audio?t=' + Date.now(), $('asrAudio'), $('asrStatus'), '识别版已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('asrStatus').textContent = error.message; } }
    async function loadAllAudio() { if (!currentId) return; const id = currentId; const seq = selectionSeq; await Promise.allSettled([loadRawAudio(id, seq), loadPreviewAudio(true, id, seq), loadAsrCleanAudio(true, id, seq)]); }
    async function postJob(action) { if (!currentId) return; const uploadToken = token(); if (!uploadToken) return; try { const res = await fetch('/jobs/' + encodeURIComponent(currentId) + '/' + action, { method: 'POST', headers: { 'X-Upload-Token': uploadToken } }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); $('currentStatus').textContent = '已加入转写队列。'; setTimeout(loadJobs, 500); } catch (error) { $('currentStatus').textContent = error.message; } }
    async function generatePreview() { if (!currentId) return; const id = currentId; const seq = selectionSeq; $('previewStatus').textContent = '正在生成试听版...'; try { const res = await fetch('/api/jobs/' + encodeURIComponent(id) + '/preview', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Upload-Token': token() }, body: JSON.stringify({ params: previewParamsFromForm() }) }); const data = await res.json(); if (seq !== selectionSeq || id !== currentId) return; if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); renderPreviewMeta(data.preview); await loadPreviewAudio(false, id, seq); } catch (error) { if (seq === selectionSeq) $('previewStatus').textContent = error.message; } }
    async function generateAsrClean() { if (!currentId) return; const id = currentId; const seq = selectionSeq; $('asrStatus').textContent = '正在生成识别版...'; try { const res = await fetch('/api/jobs/' + encodeURIComponent(id) + '/asr-clean', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Upload-Token': token() }, body: JSON.stringify({ params: asrParamsFromForm() }) }); const data = await res.json(); if (seq !== selectionSeq || id !== currentId) return; if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); renderAsrMeta(data.asrClean); await loadAsrCleanAudio(false, id, seq); } catch (error) { if (seq === selectionSeq) $('asrStatus').textContent = error.message; } }
    async function saveFeedback() { if (!currentId) return; $('feedbackStatus').textContent = '正在保存...'; try { const res = await fetch('/api/jobs/' + encodeURIComponent(currentId) + '/preview/feedback', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Upload-Token': token() }, body: JSON.stringify({ rating: $('feedbackRating').value, note: $('feedbackNote').value }) }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); $('feedbackStatus').textContent = '已保存。'; $('feedbackNote').value = ''; renderFeedback(data.feedback); } catch (error) { $('feedbackStatus').textContent = error.message; } }
    loadJobs();
  </script>
</body>
</html>`;
}

function dashboardJobHtml() {
  return `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Cardputer Job Detail</title>
  <style>
    :root { color-scheme: dark; --bg:#070b09; --panel:#101613; --soft:#0c120f; --line:#21412f; --text:#e8fff0; --muted:#8fb09b; --ok:#40ff83; --bad:#ff5d5d; --warn:#ffd166; }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--text); }
    header { border-bottom: 1px solid var(--line); background: #090f0c; position: sticky; top: 0; z-index: 2; }
    .head, main { max-width: 1180px; margin: 0 auto; padding: 16px 18px; }
    h1 { margin: 0; font-size: 22px; letter-spacing: 0; }
    h2 { margin: 0 0 10px; font-size: 15px; color: var(--ok); }
    a { color: var(--ok); }
    input, button, textarea, select { border: 1px solid var(--line); background: var(--soft); color: var(--text); border-radius: 6px; padding: 0 10px; font: inherit; min-width: 0; }
    input, button, select { height: 38px; }
    textarea { width: 100%; min-height: 78px; padding: 10px; resize: vertical; }
    button { cursor: pointer; color: var(--ok); white-space: nowrap; }
    button:hover { border-color: var(--ok); }
    button.danger { color: var(--bad); }
    .top { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 12px; }
    .bar { display: grid; grid-template-columns: minmax(220px, 1fr) auto auto auto; gap: 8px; align-items: center; }
    .panel { border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 12px; margin-top: 14px; }
    .panel.compact { padding: 10px; }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }
    .grid.two { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .summary-grid { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 10px; }
    .label { color: var(--muted); font-size: 12px; margin-bottom: 6px; }
    .value { font-size: 18px; font-weight: 700; overflow-wrap: anywhere; }
    .muted { color: var(--muted); }
    .ok { color: var(--ok); }
    .bad { color: var(--bad); }
    .warn { color: var(--warn); }
    .pill { display: inline-block; border: 1px solid var(--line); border-radius: 999px; padding: 2px 7px; white-space: nowrap; }
    .pill.ok { color: var(--ok); }
    .pill.bad { color: var(--bad); }
    .pill.warn { color: var(--warn); }
    .statusline { display: flex; gap: 10px; align-items: center; min-height: 24px; margin-top: 8px; color: var(--muted); font-size: 13px; flex-wrap: wrap; }
    .error { color: var(--bad); }
    .actions { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
    .section-title { display: flex; justify-content: space-between; gap: 10px; align-items: center; margin-bottom: 10px; }
    .audio-workbench { position: sticky; top: 94px; z-index: 1; box-shadow: 0 12px 30px rgba(0,0,0,.22); }
    .audio-players { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
    .player-card { border: 1px solid #17251d; background: #080d0a; border-radius: 8px; padding: 10px; min-width: 0; }
    .player-head { display: flex; justify-content: space-between; gap: 8px; align-items: center; min-height: 28px; }
    .player-title { font-weight: 700; }
    .player-status { color: var(--muted); font-size: 12px; overflow-wrap: anywhere; }
    .quick-actions { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .quick-actions button { height: 32px; padding: 0 9px; }
    .tuning-layout { display: grid; grid-template-columns: minmax(0, 1fr) 260px; gap: 12px; align-items: start; }
    .metrics-box { border: 1px solid #17251d; border-radius: 8px; background: #080d0a; padding: 10px; }
    .timeline { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 8px; }
    .step { border: 1px solid var(--line); border-radius: 8px; padding: 10px; background: #0c120f; min-height: 78px; }
    .step strong { display: block; margin-bottom: 6px; }
    .kv { display: grid; grid-template-columns: 140px minmax(0, 1fr); gap: 8px 12px; font-size: 14px; }
    .kv div:nth-child(odd) { color: var(--muted); }
    .kv div:nth-child(even) { overflow-wrap: anywhere; }
    .mono { font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }
    .feedback-list { display: grid; gap: 8px; margin-top: 10px; }
    .feedback-item { border: 1px solid #17251d; border-radius: 6px; padding: 10px; background: #080d0a; }
    .feedback-item strong { color: var(--ok); }
    pre { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font: 12px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace; background: #080d0a; border: 1px solid #17251d; border-radius: 6px; padding: 10px; max-height: 420px; overflow: auto; }
    audio { width: 100%; margin-top: 10px; height: 36px; }
    .hidden { display: none !important; }
    @media (max-width: 900px) { .grid, .grid.two, .summary-grid, .audio-players, .tuning-layout { grid-template-columns: repeat(2, minmax(0, 1fr)); } .bar { grid-template-columns: 1fr auto; } .audio-workbench { position: static; } }
    @media (max-width: 560px) { .grid, .grid.two, .summary-grid, .audio-players, .tuning-layout, .timeline, .kv { grid-template-columns: 1fr; } .top { align-items: flex-start; flex-direction: column; } .bar { grid-template-columns: 1fr 1fr; } #token { grid-column: 1 / -1; } }
  </style>
</head>
<body>
  <header>
    <div class="head">
      <div class="top">
        <h1 id="title">录音详情</h1>
        <a href="/dashboard">返回后台</a>
      </div>
      <div class="bar">
        <input id="token" type="password" autocomplete="off" placeholder="UPLOAD_TOKEN">
        <button id="save">保存</button>
        <button id="refresh">刷新</button>
        <button id="clear" class="danger">清除</button>
      </div>
      <div class="statusline"><span id="stamp">等待登录</span><span id="error" class="error"></span></div>
    </div>
  </header>
  <main>
    <section class="panel compact">
      <div class="summary-grid">
        <div><div class="label">状态</div><div id="status" class="value">-</div></div>
        <div><div class="label">阶段</div><div id="phase" class="value">-</div></div>
        <div><div class="label">设备</div><div id="device" class="value">-</div></div>
        <div><div class="label">大小</div><div id="bytes" class="value">-</div></div>
        <div><div class="label">格式</div><div id="encoding" class="value">-</div></div>
        <div><div class="label">录音时间</div><div id="recordedAt" class="value">-</div></div>
        <div><div class="label">创建</div><div id="createdAt" class="value">-</div></div>
        <div><div class="label">更新</div><div id="updatedAt" class="value">-</div></div>
      </div>
    </section>

    <section class="panel">
      <div class="section-title"><h2>上传 / 音频信息</h2><span id="audioSummary" class="muted"></span></div>
      <div id="uploadMeta" class="kv"></div>
    </section>

    <section class="panel">
      <div class="section-title">
        <h2>任务操作</h2>
        <span id="actionStatus" class="muted"></span>
      </div>
      <div class="actions">
        <button id="reprocess">重跑转写</button>
        <button id="resend" class="danger">重发 flomo</button>
      </div>
    </section>

    <section class="panel">
      <div class="section-title"><h2>处理时间线</h2><span id="recording" class="muted"></span></div>
      <div id="timeline" class="timeline"></div>
    </section>

    <section id="errorPanel" class="panel hidden">
      <h2>错误 / 等待原因</h2>
      <pre id="jobError"></pre>
    </section>

    <section class="panel audio-workbench" id="audioWorkbench">
      <div class="section-title">
        <div>
          <h2>试听对比台</h2>
          <div class="muted">原始录音、耳朵试听版、ASR 识别版在这里统一加载和切换。</div>
        </div>
        <div class="quick-actions">
          <button id="loadAllAudio" class="primary">加载全部</button>
          <button id="playRawNow">听原始</button>
          <button id="playPreviewNow">听试听版</button>
          <button id="playAsrNow">听识别版</button>
          <button id="pauseAllAudio" class="ghost">暂停</button>
        </div>
      </div>
      <div class="audio-players">
        <div class="player-card">
          <div class="player-head"><span class="player-title">原始 WAV</span><span id="audioStatus" class="player-status">未加载</span></div>
          <audio id="audio" controls class="hidden"></audio>
        </div>
        <div class="player-card">
          <div class="player-head"><span class="player-title">人耳试听版</span><span id="previewStatus" class="player-status">未生成</span></div>
          <audio id="previewAudio" controls class="hidden"></audio>
        </div>
        <div class="player-card">
          <div class="player-head"><span class="player-title">ASR 识别版</span><span id="asrStatus" class="player-status">未生成</span></div>
          <audio id="asrAudio" controls class="hidden"></audio>
        </div>
      </div>
    </section>

    <section class="panel">
      <h2>转写文本</h2>
      <pre id="transcript">-</pre>
    </section>

    <section class="panel">
      <h2>flomo memo</h2>
      <pre id="memo">-</pre>
    </section>

    <section class="panel">
      <div class="section-title"><h2>人耳试听版参数</h2><span class="muted">调舒服、少刮擦，不影响转写</span></div>
      <div class="tuning-layout">
        <div class="grid">
          <div><div class="label">增益</div><input id="previewGain" type="number" step="0.1" min="0.2" max="3"></div>
          <div><div class="label">低通系数</div><input id="previewLowpass" type="number" step="1" min="1" max="255"></div>
          <div><div class="label">高频保留</div><input id="previewHighMix" type="number" step="0.05" min="0" max="1"></div>
          <div><div class="label">命中保持帧</div><input id="previewHold" type="number" step="1" min="0" max="20"></div>
          <div><div class="label">RMS 上限</div><input id="previewRms" type="number" step="50" min="0" max="10000"></div>
          <div><div class="label">Diff 下限</div><input id="previewDiff" type="number" step="10" min="0" max="5000"></div>
          <div><div class="label">Diff/RMS 比例</div><input id="previewRatio" type="number" step="5" min="1" max="512"></div>
          <div><div class="label">帧采样数</div><input id="previewFrame" type="number" step="64" min="64" max="2048"></div>
          <div><div class="label">底噪 RMS 上限</div><input id="previewNoiseRms" type="number" step="25" min="0" max="5000"></div>
          <div><div class="label">底噪保留比例</div><input id="previewNoiseMix" type="number" step="0.05" min="0" max="1"></div>
        </div>
        <div class="metrics-box">
          <div class="label">处理统计</div>
          <div id="previewMeta" class="kv"></div>
        </div>
      </div>
      <div class="statusline">
        <button id="presetGentle">轻柔</button>
        <button id="presetDefault">默认</button>
        <button id="presetStrong">强力</button>
        <button id="generatePreview" class="primary">生成并试听</button>
        <button id="loadPreview">只加载</button>
      </div>
      <div class="grid two" style="margin-top:10px">
        <div>
          <div class="label">听感结论</div>
          <select id="feedbackRating">
            <option value="">选择结论</option>
            <option value="better">更好</option>
            <option value="worse">更差</option>
            <option value="mixed">有好有坏</option>
            <option value="neutral">差不多</option>
          </select>
        </div>
        <div>
          <div class="label">保存</div>
          <button id="saveFeedback">保存听感记录</button>
        </div>
      </div>
      <div style="margin-top:10px">
        <div class="label">听感备注</div>
        <textarea id="feedbackNote" placeholder="例如：刮擦少了一点，但人声变闷；强力版音量小，轻柔版更自然。"></textarea>
      </div>
      <div id="feedbackStatus" class="statusline muted"></div>
      <div id="feedbackList" class="feedback-list"></div>
      <div class="kv" style="margin-top:10px">
        <div>说明</div><div>这是服务器端模拟“小机器人耳播放版”的试听，不会覆盖原始 WAV，也不会改变 flomo/转写结果。</div>
      </div>
    </section>

    <section class="panel">
      <div class="section-title"><h2>ASR 识别版参数</h2><span class="muted">给 DashScope 用，优先清楚和稳定</span></div>
      <div class="tuning-layout">
        <div class="grid">
          <div><div class="label">增益</div><input id="asrGain" type="number" step="0.05" min="0.2" max="4"></div>
          <div><div class="label">高通系数</div><input id="asrHighpass" type="number" step="0.001" min="0" max="0.999"></div>
          <div><div class="label">预加重</div><input id="asrPreEmphasis" type="number" step="0.05" min="0" max="0.95"></div>
          <div><div class="label">噪声门 RMS</div><input id="asrNoiseGateRms" type="number" step="20" min="0" max="5000"></div>
          <div><div class="label">门限保留</div><input id="asrNoiseGateFloor" type="number" step="0.05" min="0" max="1"></div>
          <div><div class="label">压缩阈值</div><input id="asrCompressorThreshold" type="number" step="100" min="500" max="24000"></div>
          <div><div class="label">压缩比</div><input id="asrCompressorRatio" type="number" step="0.1" min="1" max="12"></div>
          <div><div class="label">目标峰值</div><input id="asrTargetPeak" type="number" step="500" min="4000" max="32000"></div>
          <div><div class="label">限幅</div><input id="asrLimiter" type="number" step="500" min="4000" max="32767"></div>
          <div><div class="label">帧采样数</div><input id="asrFrameSamples" type="number" step="80" min="80" max="2048"></div>
        </div>
        <div class="metrics-box">
          <div class="label">处理统计</div>
          <div id="asrMeta" class="kv"></div>
        </div>
      </div>
      <div class="statusline">
        <button id="asrPresetDefault">ASR 默认</button>
        <button id="generateAsrClean" class="primary">生成并试听</button>
        <button id="loadAsrClean">只加载</button>
        <button id="reprocessFromAsr">用它重跑转写</button>
      </div>
    </section>

    <section class="panel">
      <h2>文件状态</h2>
      <div id="fileMeta" class="kv"></div>
    </section>

    <section class="panel">
      <h2>任务 JSON</h2>
      <pre id="raw">-</pre>
    </section>
  </main>
  <script>
    const $ = (id) => document.getElementById(id);
    const jobId = decodeURIComponent(location.pathname.split('/').pop() || '');
    const fmtTime = (v) => v ? new Date(v).toLocaleString() : '-';
    const fmtBytes = (n) => Number.isFinite(n) ? (n > 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' KB') : '-';
    const fmtDuration = (s) => Number.isFinite(s) ? (s >= 60 ? Math.floor(s / 60) + 'm ' + Math.round(s % 60) + 's' : s.toFixed(1) + 's') : '-';
    const esc = (v) => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const normalizeToken = (value) => String(value || '').trim().replace(/^UPLOAD_TOKEN\\s*=\\s*/i, '').replace(/^token\\s*=\\s*/i, '').replace(/^['"]|['"]$/g, '').trim();
    const tokenInput = $('token');
    tokenInput.value = localStorage.getItem('cardputerUploadToken') || '';
    $('title').textContent = jobId ? '录音详情 ' + jobId : '录音详情';
    $('save').onclick = () => { tokenInput.value = normalizeToken(tokenInput.value); localStorage.setItem('cardputerUploadToken', tokenInput.value); load(); };
    $('refresh').onclick = () => load();
    $('clear').onclick = () => { localStorage.removeItem('cardputerUploadToken'); tokenInput.value = ''; load(); tokenInput.focus(); };
    $('loadAllAudio').onclick = () => loadAllAudio();
    $('playRawNow').onclick = () => playOnly($('audio'));
    $('playPreviewNow').onclick = () => playOnly($('previewAudio'));
    $('playAsrNow').onclick = () => playOnly($('asrAudio'));
    $('pauseAllAudio').onclick = () => pauseAllAudio();
    $('reprocess').onclick = () => postJob('process');
    $('reprocessFromAsr').onclick = () => postJob('process');
    $('resend').onclick = () => postJob('resend');
    $('generatePreview').onclick = () => generatePreview();
    $('loadPreview').onclick = () => loadPreviewAudio();
    $('saveFeedback').onclick = () => savePreviewFeedback();
    $('asrPresetDefault').onclick = () => setAsrParams(defaultAsrParams);
    $('generateAsrClean').onclick = () => generateAsrClean();
    $('loadAsrClean').onclick = () => loadAsrCleanAudio();
    $('presetGentle').onclick = () => setPreviewParams({ gain: 1, lowpass: 36, highMix: 0.86, scratchRmsMax: 900, scratchDiffMin: 320, scratchRatio: 170, holdFrames: 0, frameSamples: 256, noiseRmsMax: 260, noiseMix: 0.9 });
    $('presetDefault').onclick = () => setPreviewParams(defaultPreviewParams);
    $('presetStrong').onclick = () => setPreviewParams({ gain: 1, lowpass: 72, highMix: 0.55, scratchRmsMax: 1700, scratchDiffMin: 190, scratchRatio: 115, holdFrames: 1, frameSamples: 256, noiseRmsMax: 760, noiseMix: 0.62 });
    const defaultPreviewParams = {
      gain: 1,
      lowpass: 48,
      highMix: 0.72,
      scratchRmsMax: 1200,
      scratchDiffMin: 260,
      scratchRatio: 145,
      holdFrames: 0,
      frameSamples: 256,
      noiseRmsMax: 420,
      noiseMix: 0.78
    };
    const defaultAsrParams = {
      gain: 1.25,
      highpass: 0.985,
      preEmphasis: 0.18,
      noiseGateRms: 520,
      noiseGateFloor: 0.18,
      compressorThreshold: 5200,
      compressorRatio: 3.2,
      targetPeak: 26000,
      limiter: 30000,
      frameSamples: 320
    };
    setPreviewParams(defaultPreviewParams);
    setAsrParams(defaultAsrParams);

    function statusClass(status) {
      if (status === 'done' || status === 'uploaded' || status === 'transcribed') return 'ok';
      if (String(status || '').includes('failed')) return 'bad';
      return 'warn';
    }

    function statusPill(status) {
      return '<span class="pill ' + statusClass(status) + '">' + esc(status || '-') + '</span>';
    }

    function step(name, state, time, detail) {
      return '<div class="step"><strong>' + esc(name) + '</strong><div>' + statusPill(state) + '</div><div class="muted">' + esc(time || '-') + '</div><div class="muted">' + esc(detail || '') + '</div></div>';
    }

    function kv(rows) {
      return rows.map(([k, v, cls]) => '<div>' + esc(k) + '</div><div class="' + esc(cls || '') + '">' + (v || '-') + '</div>').join('');
    }

    function fileLine(file) {
      if (!file) return '-';
      if (file.error) return '<span class="bad">' + esc(file.error) + '</span>';
      return fmtBytes(file.bytes) + ' · ' + fmtTime(file.updatedAt) + (file.path ? '<br><span class="muted mono">' + esc(file.path) + '</span>' : '');
    }

    function setPreviewParams(params) {
      const p = { ...defaultPreviewParams, ...(params || {}) };
      $('previewGain').value = p.gain;
      $('previewLowpass').value = p.lowpass;
      $('previewHighMix').value = p.highMix;
      $('previewHold').value = p.holdFrames;
      $('previewRms').value = p.scratchRmsMax;
      $('previewDiff').value = p.scratchDiffMin;
      $('previewRatio').value = p.scratchRatio;
      $('previewFrame').value = p.frameSamples;
      $('previewNoiseRms').value = p.noiseRmsMax;
      $('previewNoiseMix').value = p.noiseMix;
    }

    function previewParamsFromForm() {
      return {
        gain: Number($('previewGain').value),
        lowpass: Number($('previewLowpass').value),
        highMix: Number($('previewHighMix').value),
        holdFrames: Number($('previewHold').value),
        scratchRmsMax: Number($('previewRms').value),
        scratchDiffMin: Number($('previewDiff').value),
        scratchRatio: Number($('previewRatio').value),
        frameSamples: Number($('previewFrame').value),
        noiseRmsMax: Number($('previewNoiseRms').value),
        noiseMix: Number($('previewNoiseMix').value)
      };
    }

    function setAsrParams(params) {
      const p = { ...defaultAsrParams, ...(params || {}) };
      $('asrGain').value = p.gain;
      $('asrHighpass').value = p.highpass;
      $('asrPreEmphasis').value = p.preEmphasis;
      $('asrNoiseGateRms').value = p.noiseGateRms;
      $('asrNoiseGateFloor').value = p.noiseGateFloor;
      $('asrCompressorThreshold').value = p.compressorThreshold;
      $('asrCompressorRatio').value = p.compressorRatio;
      $('asrTargetPeak').value = p.targetPeak;
      $('asrLimiter').value = p.limiter;
      $('asrFrameSamples').value = p.frameSamples;
    }

    function asrParamsFromForm() {
      return {
        gain: Number($('asrGain').value),
        highpass: Number($('asrHighpass').value),
        preEmphasis: Number($('asrPreEmphasis').value),
        noiseGateRms: Number($('asrNoiseGateRms').value),
        noiseGateFloor: Number($('asrNoiseGateFloor').value),
        compressorThreshold: Number($('asrCompressorThreshold').value),
        compressorRatio: Number($('asrCompressorRatio').value),
        targetPeak: Number($('asrTargetPeak').value),
        limiter: Number($('asrLimiter').value),
        frameSamples: Number($('asrFrameSamples').value)
      };
    }

    function renderPreviewMeta(preview) {
      if (!preview) {
        $('previewMeta').innerHTML = '';
        return;
      }
      $('previewMeta').innerHTML = kv([
        ['处理帧', esc((preview.metrics?.processedFrames ?? '-') + ' / ' + (preview.metrics?.totalFrames ?? '-'))],
        ['摩擦命中帧', esc(preview.metrics?.detectedFrames ?? '-')],
        ['底噪处理帧', esc(preview.metrics?.noiseFrames ?? '-')],
        ['时长', fmtDuration(preview.metrics?.durationSec)],
        ['生成时间', fmtTime(preview.createdAt)]
      ]);
    }

    function renderAsrMeta(asrClean) {
      if (!asrClean) {
        $('asrMeta').innerHTML = '';
        return;
      }
      $('asrMeta').innerHTML = kv([
        ['门限处理帧', esc((asrClean.metrics?.gatedFrames ?? '-') + ' / ' + (asrClean.metrics?.totalFrames ?? '-'))],
        ['压缩样本', esc(asrClean.metrics?.compressedSamples ?? '-')],
        ['限幅样本', esc(asrClean.metrics?.limitedSamples ?? '-')],
        ['峰值变化', esc((asrClean.metrics?.inputPeak ?? '-') + ' → ' + (asrClean.metrics?.outputPeak ?? '-'))],
        ['归一化增益', esc(asrClean.metrics?.normalizeGain ?? '-')],
        ['时长', fmtDuration(asrClean.metrics?.durationSec)],
        ['生成时间', fmtTime(asrClean.createdAt)]
      ]);
    }

    function ratingLabel(value) {
      return ({ better: '更好', worse: '更差', mixed: '有好有坏', neutral: '差不多' })[value] || value || '-';
    }

    function renderFeedbackList(items) {
      const list = Array.isArray(items) ? items : [];
      $('feedbackList').innerHTML = list.length ? list.map((item) => {
        const metrics = item.metrics ? '处理 ' + (item.metrics.processedFrames ?? '-') + '/' + (item.metrics.totalFrames ?? '-') + '，命中 ' + (item.metrics.detectedFrames ?? '-') : '';
        return '<div class="feedback-item"><strong>' + esc(ratingLabel(item.rating)) + '</strong>' +
          '<span class="muted"> · ' + esc(fmtTime(item.createdAt)) + '</span>' +
          '<div>' + esc(item.note || '-') + '</div>' +
          '<div class="muted">' + esc(metrics) + '</div></div>';
      }).join('') : '<div class="muted">还没有听感记录。</div>';
    }

    function render(data) {
      const job = data.job;
      const files = data.files || {};
      const wav = files.audio?.wav || {};
      const encoding = job.uploadEncoding === 'ima-adpcm' ? 'ADPCM' : 'WAV';
      if (data.preview?.params) {
        setPreviewParams(data.preview.params);
        renderPreviewMeta(data.preview);
        $('previewStatus').textContent = '已读取上次试听参数，可直接加载试听版或继续微调。';
      } else {
        renderPreviewMeta(null);
      }
      if (data.asrClean?.params) {
        setAsrParams(data.asrClean.params);
        renderAsrMeta(data.asrClean);
        $('asrStatus').textContent = '已读取上次 ASR 识别版参数。';
      } else {
        setAsrParams(defaultAsrParams);
        renderAsrMeta(null);
        $('asrStatus').textContent = '还没有 ASR 识别版。';
      }
      renderFeedbackList(data.previewFeedback);
      $('status').innerHTML = statusPill(job.status);
      $('phase').textContent = job.phase ?? '-';
      $('device').textContent = job.deviceId || '-';
      $('bytes').textContent = fmtBytes(job.bytes);
      $('encoding').textContent = encoding;
      $('recordedAt').textContent = job.recordedAt || '-';
      $('createdAt').textContent = fmtTime(job.createdAt);
      $('updatedAt').textContent = fmtTime(job.updatedAt || job.createdAt);
      $('recording').textContent = job.recordingName || job.id || '';
      $('audioSummary').textContent = wav.sampleRate ? wav.sampleRate + ' Hz · ' + wav.bitsPerSample + ' bit · ' + fmtDuration(wav.durationSec) : '';
      $('stamp').textContent = '更新 ' + fmtTime(data.time);
      const reason = job.lastError || job.pendingReason || '';
      $('errorPanel').classList.toggle('hidden', !reason);
      $('jobError').textContent = reason || '';
      $('transcript').textContent = data.transcriptText || '还没有转写文本。';
      $('memo').textContent = data.memoText || '还没有 flomo memo。';
      $('raw').textContent = JSON.stringify(job, null, 2);
      $('uploadMeta').innerHTML = kv([
        ['录音文件', esc(job.recordingName || '-')],
        ['上传格式', encoding],
        ['服务器 WAV', fmtBytes(job.bytes)],
        ['实际上传', fmtBytes(job.uploadedBytes || job.bytes)],
        ['原始大小', fmtBytes(job.originalBytes || job.bytes)],
        ['压缩倍率', job.compressionRatio ? esc(job.compressionRatio + 'x') : '-'],
        ['采样参数', wav.sampleRate ? esc(wav.sampleRate + ' Hz / ' + wav.channels + ' ch / ' + wav.bitsPerSample + ' bit') : '-'],
        ['时长', fmtDuration(wav.durationSec)]
      ]);
      $('fileMeta').innerHTML = kv([
        ['原始 WAV', fileLine(files.audio)],
        ['试听 WAV', fileLine(files.preview)],
        ['试听参数', fileLine(files.previewMeta)],
        ['ASR 识别 WAV', fileLine(files.asrClean)],
        ['ASR 识别参数', fileLine(files.asrCleanMeta)],
        ['转写文本', fileLine(files.transcript)],
        ['ASR JSON', fileLine(files.transcriptJson)],
        ['flomo memo', fileLine(files.memo)]
      ]);
      $('timeline').innerHTML = [
        step('上传', job.createdAt ? 'done' : 'waiting', fmtTime(job.createdAt), job.recordedAt || ''),
        step('ASR 清理', files.asrClean ? 'done' : 'waiting', fmtTime(data.asrClean?.createdAt), job.asrSource || ''),
        step('转写', job.transcriptPath || job.transcriptText ? 'done' : (String(job.status || '').includes('transcribe_failed') ? 'failed' : job.status), fmtTime(job.updatedAt), job.dashScopeTaskId || ''),
        step('flomo', job.flomo?.sentAt ? 'done' : (String(job.status || '').includes('flomo_failed') ? 'failed' : 'waiting'), fmtTime(job.flomo?.sentAt), job.memo?.title || ''),
        step('最近更新', job.status || '-', fmtTime(job.updatedAt || job.createdAt), reason)
      ].join('');
    }

    async function load() {
      const token = normalizeToken(tokenInput.value);
      if (tokenInput.value && tokenInput.value !== token) tokenInput.value = token;
      $('error').textContent = '';
      $('stamp').textContent = token ? '正在读取...' : '等待登录';
      if (!token) return;
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId), {
          headers: { 'X-Upload-Token': token }
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        render(data);
      } catch (error) {
        $('error').textContent = error.message === 'invalid upload token'
          ? 'token 不正确：请只粘贴等号后面的 UPLOAD_TOKEN'
          : error.message;
        $('stamp').textContent = '读取失败';
      }
    }

    async function postJob(action) {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      if (action === 'resend' && !confirm('确认重新发送 ' + jobId + ' 到 flomo？这会产生一条新的 memo。')) return;
      $('actionStatus').textContent = '正在执行...';
      try {
        const res = await fetch('/jobs/' + encodeURIComponent(jobId) + '/' + action, {
          method: 'POST',
          headers: { 'X-Upload-Token': token }
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        $('actionStatus').textContent = action === 'process' ? '已加入转写队列。' : '已重发 flomo。';
        await load();
      } catch (error) {
        $('actionStatus').textContent = error.message;
      }
    }

    function audioCurrentTime() {
      return [$('audio'), $('previewAudio'), $('asrAudio')]
        .map((audio) => audio.currentTime || 0)
        .find((time) => time > 0) || 0;
    }

    function pauseAllAudio() {
      for (const audio of [$('audio'), $('previewAudio'), $('asrAudio')]) {
        audio.pause();
      }
    }

    async function playOnly(audio) {
      if (!audio.src) return;
      const at = audioCurrentTime();
      pauseAllAudio();
      if (Number.isFinite(audio.duration) && audio.duration > 0) {
        audio.currentTime = Math.min(at, Math.max(0, audio.duration - 0.05));
      } else {
        audio.currentTime = at;
      }
      await audio.play().catch(() => {});
    }

    async function loadAllAudio() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      await Promise.allSettled([
        loadAudio(),
        loadPreviewAudio(),
        loadAsrCleanAudio()
      ]);
    }

    async function loadAudio() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('audioStatus').textContent = '正在读取 WAV...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/audio', {
          headers: { 'X-Upload-Token': token }
        });
        if (!res.ok) {
          const data = await res.json().catch(() => ({}));
          throw new Error(data.error || 'HTTP ' + res.status);
        }
        const blob = await res.blob();
        const audio = $('audio');
        if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url);
        const url = URL.createObjectURL(blob);
        audio.dataset.url = url;
        audio.src = url;
        audio.classList.remove('hidden');
        $('audioStatus').textContent = 'WAV 已加载。';
      } catch (error) {
        $('audioStatus').textContent = error.message;
      }
    }

    async function generatePreview() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('previewStatus').textContent = '正在生成试听版...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/preview', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'X-Upload-Token': token
          },
          body: JSON.stringify({ params: previewParamsFromForm() })
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        $('previewStatus').textContent = '试听版已生成。';
        renderPreviewMeta(data.preview);
        await loadPreviewAudio();
        await load();
      } catch (error) {
        $('previewStatus').textContent = error.message;
      }
    }

    async function loadPreviewAudio() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('previewStatus').textContent = '正在读取试听版...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/preview/audio', {
          headers: { 'X-Upload-Token': token }
        });
        if (!res.ok) {
          const data = await res.json().catch(() => ({}));
          throw new Error(data.error || 'HTTP ' + res.status);
        }
        const blob = await res.blob();
        const audio = $('previewAudio');
        if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url);
        const url = URL.createObjectURL(blob);
        audio.dataset.url = url;
        audio.src = url;
        audio.classList.remove('hidden');
        $('previewStatus').textContent = '试听版已加载，可以和原始录音 A/B 对比。';
      } catch (error) {
        $('previewStatus').textContent = error.message;
      }
    }

    async function generateAsrClean() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('asrStatus').textContent = '正在生成 ASR 识别版...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/asr-clean', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'X-Upload-Token': token
          },
          body: JSON.stringify({ params: asrParamsFromForm() })
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        $('asrStatus').textContent = 'ASR 识别版已生成。';
        renderAsrMeta(data.asrClean);
        await loadAsrCleanAudio();
        await load();
      } catch (error) {
        $('asrStatus').textContent = error.message;
      }
    }

    async function loadAsrCleanAudio() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('asrStatus').textContent = '正在读取 ASR 识别版...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/asr-clean/audio?t=' + Date.now(), {
          headers: { 'X-Upload-Token': token }
        });
        if (!res.ok) {
          const data = await res.json().catch(() => ({}));
          throw new Error(data.error || 'HTTP ' + res.status);
        }
        const blob = await res.blob();
        const audio = $('asrAudio');
        if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url);
        const url = URL.createObjectURL(blob);
        audio.dataset.url = url;
        audio.src = url;
        audio.classList.remove('hidden');
        $('asrStatus').textContent = 'ASR 识别版已加载，可和原始/试听版对比。';
      } catch (error) {
        $('asrStatus').textContent = error.message;
      }
    }

    async function savePreviewFeedback() {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      $('feedbackStatus').textContent = '正在保存听感记录...';
      try {
        const res = await fetch('/api/jobs/' + encodeURIComponent(jobId) + '/preview/feedback', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            'X-Upload-Token': token
          },
          body: JSON.stringify({
            rating: $('feedbackRating').value,
            note: $('feedbackNote').value
          })
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status);
        $('feedbackStatus').textContent = '已保存。';
        $('feedbackNote').value = '';
        renderFeedbackList(data.feedback);
        await load();
      } catch (error) {
        $('feedbackStatus').textContent = error.message;
      }
    }

    load();
  </script>
</body>
</html>`;
}

function publicAudioUrl(recordingName) {
  if (!PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
    return '';
  }
  return `${PUBLIC_BASE_URL}/audio/${encodeURIComponent(recordingName)}?token=${encodeURIComponent(ASR_FILE_TOKEN)}`;
}

function publicAsrAudioUrl(id) {
  if (!PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
    return '';
  }
  return `${PUBLIC_BASE_URL}/audio/${encodeURIComponent(`${id}.clean-for-asr.wav`)}?token=${encodeURIComponent(ASR_FILE_TOKEN)}`;
}

async function fetchJson(url, options) {
  const response = await fetch(url, options);
  const text = await response.text();
  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = { raw: text };
    }
  }
  if (!response.ok) {
    const message = payload?.message || payload?.code || text || `HTTP ${response.status}`;
    const error = new Error(message);
    error.payload = payload;
    error.statusCode = response.status;
    throw error;
  }
  return payload;
}

async function submitDashScopeTask(audioUrl) {
  const payload = {
    model: 'paraformer-v2',
    input: {
      file_urls: [audioUrl]
    },
    parameters: {
      channel_id: [0],
      language_hints: ['zh', 'en'],
      disfluency_removal_enabled: true,
      timestamp_alignment_enabled: false
    }
  };

  const result = await fetchJson(DASH_SCOPE_TRANSCRIPTION_URL, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${DASHSCOPE_API_KEY}`,
      'Content-Type': 'application/json',
      'X-DashScope-Async': 'enable'
    },
    body: JSON.stringify(payload)
  });

  const taskId = result?.output?.task_id;
  if (!taskId) {
    throw new Error('DashScope did not return task_id');
  }
  return taskId;
}

async function waitForDashScopeTask(taskId) {
  for (let attempt = 0; attempt < 80; attempt++) {
    const result = await fetchJson(`${DASH_SCOPE_TASK_URL}${encodeURIComponent(taskId)}`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${DASHSCOPE_API_KEY}`
      }
    });

    const output = result?.output || {};
    const status = output.task_status;
    if (status === 'SUCCEEDED') {
      const first = output.results?.[0];
      if (first?.subtask_status !== 'SUCCEEDED' || !first?.transcription_url) {
        throw new Error(first?.message || first?.code || 'DashScope subtask failed');
      }
      return {
        output,
        transcriptionUrl: first.transcription_url
      };
    }
    if (status && status !== 'PENDING' && status !== 'RUNNING') {
      throw new Error(`DashScope task ${status}`);
    }
    await sleep(1500);
  }
  throw new Error('DashScope task timed out');
}

function extractTranscriptText(transcription) {
  const texts = [];
  for (const transcript of transcription?.transcripts || []) {
    if (transcript?.text) {
      texts.push(transcript.text.trim());
    }
  }
  return texts.filter(Boolean).join('\n').trim();
}

async function downloadTranscription(transcriptionUrl) {
  return fetchJson(transcriptionUrl, { method: 'GET' });
}

function normalizeTranscriptText(text) {
  return String(text || '')
    .replace(/\r\n?/g, '\n')
    .replace(/[ \t]+/g, ' ')
    .replace(/\n{3,}/g, '\n\n')
    .replace(/([。！？!?；;])\s+/g, '$1')
    .replace(/(^|[。！？!?；;\n])\s*(嗯|呃|额)[，,、\s]+/g, '$1')
    .replace(/[，,、]\s*(嗯|呃|额)\s*[，,、]/g, '，')
    .trim();
}

function escapeRegExp(value) {
  return String(value).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

async function loadCustomTermPairs() {
  try {
    const raw = await fsp.readFile(TERMS_PATH, 'utf8');
    const parsed = JSON.parse(raw);
    if (Array.isArray(parsed)) {
      return parsed
        .filter((item) => Array.isArray(item) && item.length >= 2)
        .map(([from, to]) => [String(from), String(to)]);
    }
    if (parsed && typeof parsed === 'object') {
      return Object.entries(parsed).map(([from, to]) => [String(from), String(to)]);
    }
  } catch (error) {
    if (error.code !== 'ENOENT') {
      console.error(`Failed to load terms file ${TERMS_PATH}: ${error.message}`);
    }
  }
  return [];
}

async function applyProjectTermCorrections(text) {
  let corrected = String(text || '');
  const regexReplacements = [
    [/摘\s*computer/gi, '在 Cardputer'],
    [/card\s*puter/gi, 'Cardputer'],
    [/flomo/gi, 'flomo'],
    [/浮墨/g, 'flomo'],
    [/扶墨/g, 'flomo'],
    [/dash\s*scope/gi, 'DashScope'],
    [/para\s*former/gi, 'Paraformer'],
    [/REC[\s-]?(\d{4})/gi, 'REC_$1'],
    [/套出来/g, '掏出来'],
    [/制定为自定义/g, '自定义']
  ];

  for (const [pattern, replacement] of regexReplacements) {
    corrected = corrected.replace(pattern, replacement);
  }

  for (const [from, to] of await loadCustomTermPairs()) {
    if (from) corrected = corrected.replace(new RegExp(escapeRegExp(from), 'g'), to);
  }

  corrected = corrected.replace(/(^|[^A-Za-z])computer(?=上|里|录音|工具|这个|做|的|端|项目)/gi, '$1Cardputer');
  return corrected;
}

function splitLongSentence(sentence, maxLength) {
  const parts = [];
  let rest = sentence.trim();
  while (rest.length > maxLength) {
    let cut = -1;
    const searchStart = Math.floor(maxLength * 0.55);
    for (const mark of ['，', '、', ',', ' ']) {
      const idx = rest.lastIndexOf(mark, maxLength);
      if (idx >= searchStart) {
        cut = idx + (mark === ' ' ? 0 : 1);
        break;
      }
    }
    if (cut <= 0) cut = maxLength;
    parts.push(rest.slice(0, cut).trim());
    rest = rest.slice(cut).trim();
  }
  if (rest) parts.push(rest);
  return parts;
}

function splitTranscriptSentences(text) {
  const sentenceMatches = text.match(/[^。！？!?；;\n]+[。！？!?；;]?|\n+/g) || [text];
  const sentences = [];
  for (const item of sentenceMatches) {
    const value = item.trim();
    if (!value) continue;
    sentences.push(...splitLongSentence(value, 92));
  }
  return sentences;
}

function formatTranscriptParagraphs(sentences) {
  const paragraphs = [];
  let current = [];
  let currentLength = 0;
  for (const sentence of sentences) {
    const nextLength = currentLength + sentence.length;
    if (current.length && nextLength > 150) {
      paragraphs.push(current.join(''));
      current = [];
      currentLength = 0;
    }
    current.push(sentence);
    currentLength += sentence.length;
  }
  if (current.length) paragraphs.push(current.join(''));
  return paragraphs;
}

function stripSentenceEnd(sentence) {
  return String(sentence || '').replace(/[。！？!?；;]+$/g, '').trim();
}

function makeTranscriptSummary(sentences) {
  if (!sentences.length) return '（转写结果为空）';

  const useful = sentences
    .map(stripSentenceEnd)
    .filter((sentence) => sentence.length >= 8);
  const picked = useful.slice(0, 3);
  if (!picked.length) return useful[0] || stripSentenceEnd(sentences[0]);

  return picked
    .map((sentence) => `- ${sentence}`)
    .join('\n');
}

function makeMemoTitle(sentences, job) {
  const first = sentences.map(stripSentenceEnd).find((sentence) => sentence.length >= 6);
  if (!first) return job.id || '新录音';
  return first
    .replace(/^(介绍一下|说一下|记录一下|我想说一下|今天)?/, '')
    .replace(/[，,、].*$/, '')
    .slice(0, 22)
    .trim() || (job.id || '新录音');
}

function classifyMemoSentences(sentences) {
  const todoPattern = /(下一步|需要|要做|待办|todo|TODO|记得|提醒|回头|之后|稍后|明天|下次|准备|安排)/;
  const ideaPattern = /(想法|思路|感觉|也许|可能|可以|建议|问题是|重点|结论|方案|优化|改成|做成)/;
  const todos = [];
  const ideas = [];

  for (const sentence of sentences.map(stripSentenceEnd)) {
    if (sentence.length < 6) continue;
    if (todoPattern.test(sentence)) todos.push(sentence);
    else if (ideaPattern.test(sentence)) ideas.push(sentence);
  }

  return {
    todos: [...new Set(todos)].slice(0, 6),
    ideas: [...new Set(ideas)].slice(0, 6)
  };
}

async function buildMemoSections(text, job) {
  const corrected = await applyProjectTermCorrections(normalizeTranscriptText(text));
  if (!corrected) {
    return {
      title: job.id || '新录音',
      summary: '（转写结果为空）',
      todos: [],
      ideas: [],
      original: '（转写结果为空）'
    };
  }

  const sentences = splitTranscriptSentences(corrected);
  const paragraphs = formatTranscriptParagraphs(sentences);
  const classified = classifyMemoSentences(sentences);
  return {
    title: makeMemoTitle(sentences, job),
    summary: makeTranscriptSummary(sentences),
    todos: classified.todos,
    ideas: classified.ideas,
    original: paragraphs.join('\n\n')
  };
}

function formatBulletList(items) {
  return items.map((item) => `- ${item}`).join('\n');
}

function formatJobTime(job) {
  if (job.recordedAt) return job.recordedAt;
  return new Date(job.createdAt || Date.now()).toLocaleString('zh-CN', {
    timeZone: 'Asia/Shanghai'
  });
}

async function buildFlomoContent(job, text) {
  const memo = await buildMemoSections(text, job);
  const parts = [
    `#Cardputer语音 / ${memo.title}`,
    '',
    '## 摘要',
    memo.summary
  ];

  if (memo.todos.length) {
    parts.push('', '## 待办', formatBulletList(memo.todos));
  }
  if (memo.ideas.length) {
    parts.push('', '## 想法', formatBulletList(memo.ideas));
  }

  parts.push(
    '',
    '## 原文',
    memo.original,
    '',
    '---',
    `录音：${job.recordingName}`,
    `设备：${job.deviceId}`,
    `时间：${formatJobTime(job)}`,
    '来源：Cardputer 自动转写'
  );

  return {
    content: parts.join('\n'),
    memo
  };
}

async function sendToFlomo(job, text) {
  const memoPayload = await buildFlomoContent(job, text);
  const result = await fetchJson(FLOMO_WEBHOOK_URL, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({ content: memoPayload.content })
  });

  if (typeof result?.code === 'number' && result.code !== 0) {
    throw new Error(result?.message || `flomo returned code ${result.code}`);
  }
  return {
    result,
    content: memoPayload.content,
    memo: memoPayload.memo
  };
}

async function processJob(id) {
  if (processingJobs.has(id)) {
    return;
  }
  processingJobs.add(id);
  try {
    let job = await readJob(id);
    if (['done', 'sent_to_flomo'].includes(job.status)) {
      return;
    }
    if (!DASHSCOPE_API_KEY || !PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
      job.status = 'uploaded';
      job.pendingReason = 'transcription is not configured';
      await writeJob(job);
      return;
    }

    const { meta: asrCleanMeta } = await writeAsrCleanForJob(job, job.asrCleanParams);
    job.asrCleanPath = path.relative(DATA_ROOT, asrCleanPathForId(id));
    job.asrCleanMetaPath = path.relative(DATA_ROOT, asrCleanMetaPathForId(id));
    job.asrClean = {
      createdAt: asrCleanMeta.createdAt,
      params: asrCleanMeta.params,
      metrics: asrCleanMeta.metrics
    };
    await writeJob(job);

    const audioUrl = publicAsrAudioUrl(id);
    job.status = 'transcribing';
    job.phase = 3;
    job.audioUrl = audioUrl.replace(ASR_FILE_TOKEN, '***');
    job.asrSource = 'clean-for-asr';
    delete job.lastError;
    await writeJob(job);

    const taskId = await submitDashScopeTask(audioUrl);
    job = await readJob(id);
    job.dashScopeTaskId = taskId;
    await writeJob(job);

    const { output, transcriptionUrl } = await waitForDashScopeTask(taskId);
    const transcription = await downloadTranscription(transcriptionUrl);
    const text = extractTranscriptText(transcription);
    const transcriptTextPath = path.join(TRANSCRIPT_DIR, `${id}.txt`);
    const transcriptJsonPath = path.join(TRANSCRIPT_DIR, `${id}.json`);
    await fsp.writeFile(transcriptTextPath, `${text}\n`, 'utf8');
    await fsp.writeFile(transcriptJsonPath, `${JSON.stringify(transcription, null, 2)}\n`, 'utf8');

    job = await readJob(id);
    job.status = 'transcribed';
    job.phase = 3;
    job.transcriptPath = path.relative(DATA_ROOT, transcriptTextPath);
    job.transcriptJsonPath = path.relative(DATA_ROOT, transcriptJsonPath);
    job.transcriptText = text;
    job.dashScope = {
      taskId,
      usage: output.usage,
      taskMetrics: output.task_metrics
    };
    await writeJob(job);

    if (!FLOMO_WEBHOOK_URL) {
      job.status = 'transcribed';
      job.pendingReason = 'flomo is not configured';
      await writeJob(job);
      return;
    }

    const flomoPayload = await sendToFlomo(job, text);
    const transcriptMemoPath = transcriptMemoPathForId(id);
    await fsp.writeFile(transcriptMemoPath, `${flomoPayload.content}\n`, 'utf8');
    job = await readJob(id);
    job.status = 'done';
    job.phase = 4;
    job.transcriptMemoPath = path.relative(DATA_ROOT, transcriptMemoPath);
    job.memo = {
      title: flomoPayload.memo.title,
      hasTodos: flomoPayload.memo.todos.length > 0,
      hasIdeas: flomoPayload.memo.ideas.length > 0
    };
    job.flomo = {
      sentAt: new Date().toISOString(),
      memoSlug: flomoPayload.result?.memo?.slug
    };
    delete job.pendingReason;
    await writeJob(job);
  } catch (error) {
    const job = await readJob(id).catch(() => null);
    if (job) {
      job.status = job.status === 'transcribing' ? 'transcribe_failed' : 'flomo_failed';
      job.lastError = error.message;
      await writeJob(job).catch(() => {});
    }
    console.error(`Job ${id} failed: ${error.message}`);
  } finally {
    processingJobs.delete(id);
  }
}

function scheduleProcessJob(id) {
  setTimeout(() => {
    processJob(id).catch((error) => {
      console.error(`Job ${id} failed: ${error.message}`);
    });
  }, 0);
}

async function handleUpload(req, res) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }

  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const contentType = String(req.headers['content-type'] || '').toLowerCase();
  const requestedEncoding = String(req.headers['x-audio-encoding'] || '').toLowerCase();
  const isAdpcmUpload = requestedEncoding === 'ima-adpcm' || contentType.includes('application/x-cardputer-adpcm');
  if (!isAdpcmUpload && !contentType.includes('audio/wav') && !contentType.includes('audio/x-wav')) {
    sendJson(res, 415, { ok: false, error: 'Content-Type must be audio/wav or application/x-cardputer-adpcm' });
    return;
  }

  const rawDeviceId = String(req.headers['x-device-id'] || '').trim();
  const rawRecordingName = String(req.headers['x-recording-name'] || '').trim();
  const rawRecordedAt = String(req.headers['x-recorded-at'] || '').trim();
  if (!rawDeviceId || !rawRecordingName) {
    sendJson(res, 400, {
      ok: false,
      error: 'X-Device-Id and X-Recording-Name are required'
    });
    return;
  }

  const normalized = normalizeRecordingName(rawRecordingName);
  if (!normalized) {
    sendJson(res, 400, { ok: false, error: 'invalid recording name' });
    return;
  }

  const deviceId = rawDeviceId.replace(/[^\w.-]/g, '_').slice(0, 80);
  const { id: jobId, recordingName } = normalized;
  const uploadPath = path.join(UPLOAD_DIR, recordingName);
  const jobPath = path.join(JOB_DIR, `${jobId}.json`);
  const startedAt = new Date().toISOString();
  const uploadProgress = {
    id: jobId,
    deviceId,
    recordingName,
    encoding: isAdpcmUpload ? 'ima-adpcm' : 'wav',
    originalBytes: parseHeaderInt(req.headers['x-original-content-length']),
    bytesReceived: 0,
    totalBytes: parseHeaderInt(req.headers['content-length']),
    startedAt,
    updatedAt: startedAt
  };
  const wifiRssi = parseHeaderInt(req.headers['x-wifi-rssi']);
  const wifiIp = String(req.headers['x-wifi-ip'] || '').replace(/[^\d.:a-fA-F]/g, '').slice(0, 48);
  updateDeviceStatus(deviceId, {
    lastStatus: 'uploading',
    lastRecordingName: recordingName,
    wifiRssi,
    wifiIp
  });

  const uploadExists = fs.existsSync(uploadPath);
  const jobExists = fs.existsSync(jobPath);
  if (jobExists) {
    req.resume();
    updateDeviceStatus(deviceId, {
      lastStatus: 'duplicate',
      lastRecordingName: recordingName,
      wifiRssi,
      wifiIp
    });
    scheduleProcessJob(jobId);
    sendJson(res, 200, { ok: true, id: jobId, duplicate: true });
    return;
  }
  if (uploadExists) {
    await fsp.rm(uploadPath, { force: true }).catch(() => {});
    updateDeviceStatus(deviceId, {
      lastStatus: 'retrying_stale_upload',
      lastRecordingName: recordingName,
      wifiRssi,
      wifiIp
    });
  }

  let bytes = 0;
  let uploadedBytes = 0;
  let out = null;
  activeUploads.set(jobId, uploadProgress);

  try {
    if (isAdpcmUpload) {
      const encoded = await collectRequestBody(req, MAX_UPLOAD_BYTES, (received) => {
        uploadedBytes = received;
        uploadProgress.bytesReceived = received;
        uploadProgress.updatedAt = new Date().toISOString();
      });
      const pcmBytes = parseHeaderInt(req.headers['x-adpcm-pcm-bytes']);
      const wavBuffer = decodeImaAdpcmToWav(encoded, {
        sampleRate: parseHeaderInt(req.headers['x-adpcm-sample-rate']) || 16000,
        pcmBytes,
        predictor: parseHeaderInt(req.headers['x-adpcm-initial-predictor']),
        index: parseHeaderInt(req.headers['x-adpcm-initial-index']) || 0
      });
      bytes = wavBuffer.length;
      await fsp.writeFile(uploadPath, wavBuffer, { flag: 'wx' });
    } else {
      out = fs.createWriteStream(uploadPath, { flags: 'wx' });
      req.on('data', (chunk) => {
        bytes += chunk.length;
        uploadedBytes = bytes;
        uploadProgress.bytesReceived = bytes;
        uploadProgress.updatedAt = new Date().toISOString();
        if (bytes > MAX_UPLOAD_BYTES) {
          req.destroy(new Error('upload too large'));
        }
      });

      req.pipe(out);
      await new Promise((resolve, reject) => {
        req.on('error', reject);
        out.on('error', reject);
        out.on('finish', resolve);
      });
    }

    const job = {
      id: jobId,
      status: 'uploaded',
      phase: 1,
      deviceId,
      recordingName,
      recordedAt: rawRecordedAt.slice(0, 40),
      bytes,
      uploadEncoding: isAdpcmUpload ? 'ima-adpcm' : 'wav',
      uploadedBytes: uploadedBytes || bytes,
      originalBytes: bytes,
      compressionRatio: isAdpcmUpload && uploadedBytes ? Number((bytes / uploadedBytes).toFixed(2)) : 1,
      uploadPath: path.relative(DATA_ROOT, uploadPath),
      createdAt: startedAt,
      updatedAt: new Date().toISOString()
    };

    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');
    updateDeviceStatus(deviceId, {
      lastStatus: 'server_received',
      lastRecordingName: recordingName,
      lastJobId: jobId,
      wifiRssi,
      wifiIp
    });
    scheduleProcessJob(jobId);
    sendJson(res, 201, { ok: true, id: jobId });
  } catch (error) {
    if (out) out.destroy();
    await fsp.rm(uploadPath, { force: true }).catch(() => {});
    updateDeviceStatus(deviceId, {
      lastStatus: error.message === 'upload too large' ? 'upload_too_large' : 'upload_failed',
      lastRecordingName: recordingName,
      lastError: error.message,
      wifiRssi,
      wifiIp
    });
    const statusCode = error.message === 'upload too large' ? 413 : 500;
    sendJson(res, statusCode, { ok: false, error: error.message });
  } finally {
    activeUploads.delete(jobId);
  }
}

async function handleJobsList(req, res, url) {
  if (!dashboardAuth(req, res)) return;

  const rawLimit = parseInt(url.searchParams.get('limit') || '20', 10);
  const limit = Math.max(1, Math.min(Number.isFinite(rawLimit) ? rawLimit : 20, 100));
  const status = String(url.searchParams.get('status') || '').trim();
  if (status && !/^[\w.-]+$/.test(status)) {
    sendJson(res, 400, { ok: false, error: 'invalid status filter' });
    return;
  }

  try {
    sendJson(res, 200, { ok: true, ...(await listJobs({ limit, status })) });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleDashboardApi(req, res, url) {
  if (!dashboardAuth(req, res)) return;
  const rawLimit = parseInt(url.searchParams.get('limit') || '50', 10);
  const limit = Math.max(1, Math.min(Number.isFinite(rawLimit) ? rawLimit : 50, 100));
  const status = String(url.searchParams.get('status') || '').trim();
  if (status && !/^[\w.-]+$/.test(status)) {
    sendJson(res, 400, { ok: false, error: 'invalid status filter' });
    return;
  }
  try {
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      health: {
        ok: true,
        service: 'cardputer-cloud-voice-server',
        configured: {
          uploadToken: Boolean(UPLOAD_TOKEN),
          publicBaseUrl: Boolean(PUBLIC_BASE_URL),
          asrFileToken: Boolean(ASR_FILE_TOKEN),
          dashScope: Boolean(DASHSCOPE_API_KEY),
          flomo: Boolean(FLOMO_WEBHOOK_URL)
        }
      },
      activeUploads: [...activeUploads.values()],
      devices: [...deviceStatuses.values()].sort((a, b) => {
        const left = Date.parse(a.updatedAt || '') || 0;
        const right = Date.parse(b.updatedAt || '') || 0;
        return right - left;
      }),
      jobs: await listJobs({ limit, status })
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handlePreviewFeedbackApi(req, res, url) {
  if (!dashboardAuth(req, res)) return;
  const rawLimit = parseInt(url.searchParams.get('limit') || '50', 10);
  const limit = Math.max(1, Math.min(Number.isFinite(rawLimit) ? rawLimit : 50, 200));

  try {
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      feedback: await collectPreviewFeedback({ limit })
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

function isValidJobId(id) {
  return /^[\w.-]+$/.test(id);
}

function resolveDataPath(relativePath) {
  if (!relativePath) return '';
  const root = path.resolve(DATA_ROOT);
  const resolved = path.resolve(DATA_ROOT, relativePath);
  if (resolved !== root && !resolved.startsWith(`${root}${path.sep}`)) {
    throw new Error('invalid data path');
  }
  return resolved;
}

async function readOptionalDataText(relativePath, maxBytes = 512 * 1024) {
  if (!relativePath) return '';
  try {
    const filePath = resolveDataPath(relativePath);
    const stat = await fsp.stat(filePath);
    if (!stat.isFile()) return '';
    if (stat.size > maxBytes) {
      return `文件太大，未在详情页内展示（${stat.size} bytes）。`;
    }
    return await fsp.readFile(filePath, 'utf8');
  } catch (error) {
    if (error.code === 'ENOENT') return '';
    return `读取失败：${error.message}`;
  }
}

async function readOptionalDataJson(relativePath, maxBytes = 512 * 1024) {
  const text = await readOptionalDataText(relativePath, maxBytes);
  if (!text || text.startsWith('文件太大') || text.startsWith('读取失败')) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

async function readPreviewFeedback(id) {
  const relativePath = path.relative(DATA_ROOT, previewFeedbackPathForId(id));
  const feedback = await readOptionalDataJson(relativePath);
  return Array.isArray(feedback) ? feedback : [];
}

async function writePreviewFeedback(id, feedback) {
  await fsp.writeFile(previewFeedbackPathForId(id), `${JSON.stringify(feedback, null, 2)}\n`, 'utf8');
}

async function collectPreviewFeedback({ limit = 50 } = {}) {
  const entries = await fsp.readdir(JOB_DIR, { withFileTypes: true });
  const rows = [];

  await Promise.all(entries.map(async (entry) => {
    if (!entry.isFile() || !entry.name.endsWith('.json')) return;
    const id = entry.name.slice(0, -'.json'.length);
    if (!isValidJobId(id)) return;
    try {
      const job = await readJob(id);
      const previewFeedback = await readPreviewFeedback(id);
      if (!previewFeedback.length) return;
      const preview = await readOptionalDataJson(path.relative(DATA_ROOT, previewMetaPathForId(id)));
      const wav = await inspectWavFile(audioPathForJob(job)).catch(() => null);
      for (const item of previewFeedback) {
        rows.push({
          job: summarizeJob(job),
          wav,
          preview,
          feedback: item
        });
      }
    } catch {
      // Keep this endpoint useful even if one old job has a broken sidecar file.
    }
  }));

  rows.sort((a, b) => {
    const left = Date.parse(a.feedback?.createdAt || '') || 0;
    const right = Date.parse(b.feedback?.createdAt || '') || 0;
    return right - left;
  });

  return rows.slice(0, limit);
}

async function statOptionalDataFile(relativePath) {
  if (!relativePath) return null;
  try {
    const filePath = resolveDataPath(relativePath);
    const stat = await fsp.stat(filePath);
    if (!stat.isFile()) return null;
    return {
      path: relativePath,
      bytes: stat.size,
      updatedAt: stat.mtime.toISOString()
    };
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    return {
      path: relativePath,
      error: error.message
    };
  }
}

async function inspectWavFile(filePath) {
  const stat = await fsp.stat(filePath);
  if (!stat.isFile()) return null;
  const handle = await fsp.open(filePath, 'r');
  try {
    const header = Buffer.alloc(44);
    const { bytesRead } = await handle.read(header, 0, header.length, 0);
    const info = {
      bytes: stat.size,
      updatedAt: stat.mtime.toISOString()
    };
    if (
      bytesRead >= 44 &&
      header.toString('ascii', 0, 4) === 'RIFF' &&
      header.toString('ascii', 8, 12) === 'WAVE'
    ) {
      const channels = header.readUInt16LE(22);
      const sampleRate = header.readUInt32LE(24);
      const bitsPerSample = header.readUInt16LE(34);
      const dataBytes = header.readUInt32LE(40);
      const bytesPerSampleFrame = Math.max(1, channels * (bitsPerSample / 8));
      info.wav = {
        channels,
        sampleRate,
        bitsPerSample,
        dataBytes,
        durationSec: sampleRate > 0 ? dataBytes / bytesPerSampleFrame / sampleRate : 0
      };
    }
    return info;
  } finally {
    await handle.close();
  }
}

function parsePcm16Wav(buffer) {
  if (
    buffer.length < 44 ||
    buffer.toString('ascii', 0, 4) !== 'RIFF' ||
    buffer.toString('ascii', 8, 12) !== 'WAVE'
  ) {
    throw new Error('not a WAV file');
  }

  let offset = 12;
  let fmt = null;
  let data = null;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString('ascii', offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const start = offset + 8;
    const end = start + size;
    if (end > buffer.length) break;
    if (id === 'fmt ' && size >= 16) {
      fmt = {
        audioFormat: buffer.readUInt16LE(start),
        channels: buffer.readUInt16LE(start + 2),
        sampleRate: buffer.readUInt32LE(start + 4),
        bitsPerSample: buffer.readUInt16LE(start + 14)
      };
    } else if (id === 'data') {
      data = { offset: start, bytes: size };
      break;
    }
    offset = end + (size % 2);
  }

  if (!fmt || !data) throw new Error('WAV fmt/data chunk missing');
  if (fmt.audioFormat !== 1 || fmt.channels !== 1 || fmt.bitsPerSample !== 16) {
    throw new Error('only 16-bit mono PCM WAV is supported for preview');
  }
  if (data.bytes % 2 !== 0) {
    throw new Error('invalid PCM byte length');
  }
  return { ...fmt, dataOffset: data.offset, dataBytes: data.bytes };
}

function normalizePreviewParams(input = {}) {
  const p = { ...DEFAULT_PREVIEW_PARAMS, ...(input || {}) };
  const number = (key, min, max) => {
    const n = Number(p[key]);
    p[key] = Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : DEFAULT_PREVIEW_PARAMS[key];
  };
  number('gain', 0.2, 3);
  number('lowpass', 1, 255);
  number('highMix', 0, 1);
  number('scratchRmsMax', 0, 10000);
  number('scratchDiffMin', 0, 5000);
  number('scratchRatio', 1, 512);
  number('holdFrames', 0, 20);
  number('frameSamples', 64, 2048);
  number('noiseRmsMax', 0, 5000);
  number('noiseMix', 0, 1);
  p.lowpass = Math.round(p.lowpass);
  p.scratchRmsMax = Math.round(p.scratchRmsMax);
  p.scratchDiffMin = Math.round(p.scratchDiffMin);
  p.scratchRatio = Math.round(p.scratchRatio);
  p.holdFrames = Math.round(p.holdFrames);
  p.frameSamples = Math.round(p.frameSamples);
  p.noiseRmsMax = Math.round(p.noiseRmsMax);
  return p;
}

function normalizeAsrParams(input = {}) {
  const p = { ...DEFAULT_ASR_PARAMS, ...(input || {}) };
  const number = (key, min, max) => {
    const n = Number(p[key]);
    p[key] = Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : DEFAULT_ASR_PARAMS[key];
  };
  number('gain', 0.2, 4);
  number('highpass', 0, 0.999);
  number('preEmphasis', 0, 0.95);
  number('noiseGateRms', 0, 5000);
  number('noiseGateFloor', 0, 1);
  number('compressorThreshold', 500, 24000);
  number('compressorRatio', 1, 12);
  number('targetPeak', 4000, 32000);
  number('limiter', 4000, 32767);
  number('frameSamples', 80, 2048);
  p.noiseGateRms = Math.round(p.noiseGateRms);
  p.compressorThreshold = Math.round(p.compressorThreshold);
  p.targetPeak = Math.round(p.targetPeak);
  p.limiter = Math.round(p.limiter);
  p.frameSamples = Math.round(p.frameSamples);
  return p;
}

function frameStats(buffer, start, samples) {
  let sumSq = 0;
  let sumDiff = 0;
  let prev = buffer.readInt16LE(start);
  for (let i = 0; i < samples; i++) {
    const sample = buffer.readInt16LE(start + i * 2);
    sumSq += sample * sample;
    if (i > 0) sumDiff += Math.abs(sample - prev);
    prev = sample;
  }
  return {
    rms: Math.sqrt(sumSq / Math.max(1, samples)),
    avgDiff: samples > 1 ? sumDiff / (samples - 1) : 0
  };
}

function buildPlayPreviewWav(wavBuffer, params) {
  const wav = parsePcm16Wav(wavBuffer);
  const p = normalizePreviewParams(params);
  const output = Buffer.alloc(44 + wav.dataBytes);
  writeWavHeaderBuffer(output, wav.sampleRate, wav.dataBytes);

  let lp = wavBuffer.readInt16LE(wav.dataOffset);
  let hold = 0;
  let detectedFrames = 0;
  let noiseFrames = 0;
  let processedFrames = 0;
  let scratchWet = 0;
  let noiseWet = 0;
  const frameBytes = p.frameSamples * 2;
  const end = wav.dataOffset + wav.dataBytes;
  let outOffset = 44;

  for (let frameStart = wav.dataOffset; frameStart < end; frameStart += frameBytes) {
    const frameSamples = Math.floor(Math.min(frameBytes, end - frameStart) / 2);
    const stats = frameStats(wavBuffer, frameStart, frameSamples);
    const detected = stats.rms > 0 &&
      stats.rms < p.scratchRmsMax &&
      stats.avgDiff > p.scratchDiffMin &&
      stats.avgDiff * 256 > stats.rms * p.scratchRatio;
    if (detected) {
      detectedFrames++;
      hold = p.holdFrames;
    }
    const active = detected || hold > 0;
    if (hold > 0) hold--;

    const quiet = p.noiseRmsMax > 0 && stats.rms > 0 && stats.rms < p.noiseRmsMax;
    if (quiet) noiseFrames++;
    if (active || quiet) processedFrames++;

    const scratchTarget = active ? 1 : 0;
    const quietRatio = quiet ? 1 - (stats.rms / Math.max(1, p.noiseRmsMax)) : 0;
    const noiseTarget = Math.max(0, Math.min(1, quietRatio));
    const scratchStep = (scratchTarget > scratchWet ? 0.018 : 0.006);
    const noiseStep = (noiseTarget > noiseWet ? 0.006 : 0.002);

    for (let i = 0; i < frameSamples; i++) {
      let sample = clampInt16(Math.round(wavBuffer.readInt16LE(frameStart + i * 2) * p.gain));
      lp += Math.round((sample - lp) * p.lowpass / 256);

      scratchWet += (scratchTarget - scratchWet) * scratchStep;
      noiseWet += (noiseTarget - noiseWet) * noiseStep;

      if (scratchWet > 0.001) {
        const hi = sample - lp;
        const scratchMix = 1 - scratchWet * (1 - p.highMix);
        sample = clampInt16(Math.round(lp + hi * scratchMix));
      }

      if (noiseWet > 0.001 && p.noiseMix < 1) {
        const absSample = Math.abs(sample);
        const body = p.noiseRmsMax > 0
          ? Math.min(1, absSample / Math.max(1, p.noiseRmsMax * 3))
          : 1;
        const protection = body * body;
        const attenuation = p.noiseMix + (1 - p.noiseMix) * protection;
        const smoothAttenuation = 1 - noiseWet * (1 - attenuation);
        sample = clampInt16(Math.round(sample * smoothAttenuation));
      }

      output.writeInt16LE(sample, outOffset);
      outOffset += 2;
    }
  }

  return {
    buffer: output,
    params: p,
    metrics: {
      detectedFrames,
      noiseFrames,
      processedFrames,
      totalFrames: Math.ceil((wav.dataBytes / 2) / p.frameSamples),
      durationSec: wav.dataBytes / 2 / wav.sampleRate,
      sampleRate: wav.sampleRate,
      algorithm: 'smooth-play-preview-v2'
    }
  };
}

function buildAsrCleanWav(wavBuffer, params) {
  const wav = parsePcm16Wav(wavBuffer);
  const p = normalizeAsrParams(params);
  const sampleCount = wav.dataBytes / 2;
  const scratch = new Int16Array(sampleCount);
  const output = Buffer.alloc(44 + wav.dataBytes);
  writeWavHeaderBuffer(output, wav.sampleRate, wav.dataBytes);

  const frameBytes = p.frameSamples * 2;
  const end = wav.dataOffset + wav.dataBytes;
  const frameGains = [];
  let gatedFrames = 0;
  let compressedSamples = 0;
  let limitedSamples = 0;
  let inputPeak = 0;
  let processedPeak = 0;
  let hp = 0;
  let prevInput = 0;
  let prevEmphasis = 0;
  let sampleIndex = 0;

  for (let frameStart = wav.dataOffset; frameStart < end; frameStart += frameBytes) {
    const frameSamples = Math.floor(Math.min(frameBytes, end - frameStart) / 2);
    const stats = frameStats(wavBuffer, frameStart, frameSamples);
    const gateGain = p.noiseGateRms > 0 && stats.rms < p.noiseGateRms
      ? p.noiseGateFloor + (1 - p.noiseGateFloor) * Math.pow(stats.rms / Math.max(1, p.noiseGateRms), 2)
      : 1;
    if (gateGain < 0.999) gatedFrames++;
    frameGains.push(gateGain);
  }

  let frameIndex = 0;
  let samplesInFrame = 0;
  for (let offset = wav.dataOffset; offset < end; offset += 2) {
    if (samplesInFrame >= p.frameSamples) {
      frameIndex++;
      samplesInFrame = 0;
    }
    const raw = wavBuffer.readInt16LE(offset);
    inputPeak = Math.max(inputPeak, Math.abs(raw));
    hp = raw - prevInput + p.highpass * hp;
    prevInput = raw;
    let sample = hp - prevEmphasis * p.preEmphasis;
    prevEmphasis = hp;
    sample *= p.gain * (frameGains[frameIndex] ?? 1);

    const sign = sample < 0 ? -1 : 1;
    const absSample = Math.abs(sample);
    if (absSample > p.compressorThreshold) {
      sample = sign * (p.compressorThreshold + (absSample - p.compressorThreshold) / p.compressorRatio);
      compressedSamples++;
    }
    if (Math.abs(sample) > p.limiter) {
      sample = sign * p.limiter;
      limitedSamples++;
    }
    const rounded = clampInt16(Math.round(sample));
    processedPeak = Math.max(processedPeak, Math.abs(rounded));
    scratch[sampleIndex++] = rounded;
    samplesInFrame++;
  }

  const normalizeGain = processedPeak > 0 ? Math.min(8, p.targetPeak / processedPeak) : 1;
  let outOffset = 44;
  let outputPeak = 0;
  for (let i = 0; i < scratch.length; i++) {
    const sample = clampInt16(Math.round(scratch[i] * normalizeGain));
    outputPeak = Math.max(outputPeak, Math.abs(sample));
    output.writeInt16LE(sample, outOffset);
    outOffset += 2;
  }

  return {
    buffer: output,
    params: p,
    metrics: {
      gatedFrames,
      totalFrames: frameGains.length,
      compressedSamples,
      limitedSamples,
      inputPeak,
      processedPeak,
      outputPeak,
      normalizeGain: Number(normalizeGain.toFixed(3)),
      durationSec: sampleCount / wav.sampleRate,
      sampleRate: wav.sampleRate
    }
  };
}

async function writeAsrCleanForJob(job, params) {
  const sourcePath = audioPathForJob(job);
  const source = await fsp.readFile(sourcePath);
  const clean = buildAsrCleanWav(source, params);
  const cleanPath = asrCleanPathForId(job.id);
  const metaPath = asrCleanMetaPathForId(job.id);
  const meta = {
    id: job.id,
    recordingName: job.recordingName,
    kind: 'clean-for-asr',
    params: clean.params,
    metrics: clean.metrics,
    sourceBytes: source.length,
    outputBytes: clean.buffer.length,
    createdAt: new Date().toISOString()
  };
  await fsp.writeFile(cleanPath, clean.buffer);
  await fsp.writeFile(metaPath, `${JSON.stringify(meta, null, 2)}\n`, 'utf8');
  return { clean, meta };
}

async function inspectJobFiles(job, memoPath) {
  const audioInfo = job.recordingName ? await inspectWavFile(audioPathForJob(job)).catch((error) => ({
    error: error.code === 'ENOENT' ? 'audio not found' : error.message
  })) : null;
  const previewRelative = path.relative(DATA_ROOT, previewPathForId(job.id));
  const previewMetaRelative = path.relative(DATA_ROOT, previewMetaPathForId(job.id));
  const previewFeedbackRelative = path.relative(DATA_ROOT, previewFeedbackPathForId(job.id));
  const asrCleanRelative = job.asrCleanPath || path.relative(DATA_ROOT, asrCleanPathForId(job.id));
  const asrCleanMetaRelative = job.asrCleanMetaPath || path.relative(DATA_ROOT, asrCleanMetaPathForId(job.id));
  return {
    audio: audioInfo,
    preview: await statOptionalDataFile(previewRelative),
    previewMeta: await statOptionalDataFile(previewMetaRelative),
    previewFeedback: await statOptionalDataFile(previewFeedbackRelative),
    asrClean: await statOptionalDataFile(asrCleanRelative),
    asrCleanMeta: await statOptionalDataFile(asrCleanMetaRelative),
    transcript: await statOptionalDataFile(job.transcriptPath),
    transcriptJson: await statOptionalDataFile(job.transcriptJsonPath),
    memo: await statOptionalDataFile(memoPath)
  };
}

function redactJobForDashboard(job) {
  const copy = JSON.parse(JSON.stringify(job));
  if (copy.audioUrl) {
    copy.audioUrl = String(copy.audioUrl).replace(/token=[^&]+/i, 'token=***');
  }
  if (typeof copy.transcriptText === 'string') {
    copy.transcriptText = `[shown above, ${copy.transcriptText.length} chars]`;
  }
  return copy;
}

async function handleJobDetailApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const job = await readJob(id);
    const transcriptText = job.transcriptText || await readOptionalDataText(job.transcriptPath);
    const memoPath = job.transcriptMemoPath || path.relative(DATA_ROOT, transcriptMemoPathForId(id));
    const memoText = await readOptionalDataText(memoPath);
    const files = await inspectJobFiles(job, memoPath);
    const preview = await readOptionalDataJson(path.relative(DATA_ROOT, previewMetaPathForId(id)));
    const asrClean = await readOptionalDataJson(job.asrCleanMetaPath || path.relative(DATA_ROOT, asrCleanMetaPathForId(id)));
    const previewFeedback = await readPreviewFeedback(id);
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      job: redactJobForDashboard(job),
      transcriptText,
      memoText,
      files,
      preview,
      asrClean,
      previewFeedback
    });
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'job not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobAudioApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/audio'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const job = await readJob(id);
    const normalized = normalizeRecordingName(job.recordingName);
    if (!normalized || normalized.recordingName !== job.recordingName) {
      sendJson(res, 400, { ok: false, error: 'invalid audio name' });
      return;
    }
    const uploadPath = audioPathForJob(job);
    const stat = await fsp.stat(uploadPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store'
    });
    fs.createReadStream(uploadPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobPreviewApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/preview'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const payload = await readJsonBody(req);
    const job = await readJob(id);
    const sourcePath = audioPathForJob(job);
    const source = await fsp.readFile(sourcePath);
    const preview = buildPlayPreviewWav(source, payload.params);
    const previewPath = previewPathForId(id);
    const metaPath = previewMetaPathForId(id);
    const meta = {
      id,
      recordingName: job.recordingName,
      kind: 'play-preview',
      params: preview.params,
      metrics: preview.metrics,
      sourceBytes: source.length,
      outputBytes: preview.buffer.length,
      createdAt: new Date().toISOString()
    };
    await fsp.writeFile(previewPath, preview.buffer);
    await fsp.writeFile(metaPath, `${JSON.stringify(meta, null, 2)}\n`, 'utf8');
    sendJson(res, 201, { ok: true, preview: meta });
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio or job not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobPreviewAudioApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/preview/audio'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const previewPath = previewPathForId(id);
    const previewMeta = await readOptionalDataJson(path.relative(DATA_ROOT, previewMetaPathForId(id)));
    if (previewMeta?.metrics?.algorithm !== 'smooth-play-preview-v2') {
      sendJson(res, 409, { ok: false, error: 'old preview; regenerate play preview' });
      return;
    }
    const stat = await fsp.stat(previewPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store'
    });
    fs.createReadStream(previewPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'preview not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobAsrCleanApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/asr-clean'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const payload = await readJsonBody(req);
    let job = await readJob(id);
    const { meta } = await writeAsrCleanForJob(job, payload.params || job.asrCleanParams);
    job = await readJob(id);
    job.asrCleanPath = path.relative(DATA_ROOT, asrCleanPathForId(id));
    job.asrCleanMetaPath = path.relative(DATA_ROOT, asrCleanMetaPathForId(id));
    job.asrCleanParams = meta.params;
    job.asrClean = {
      createdAt: meta.createdAt,
      params: meta.params,
      metrics: meta.metrics
    };
    await writeJob(job);
    sendJson(res, 201, { ok: true, asrClean: meta });
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio or job not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobAsrCleanAudioApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/asr-clean/audio'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const cleanPath = asrCleanPathForId(id);
    const stat = await fsp.stat(cleanPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store'
    });
    fs.createReadStream(cleanPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'ASR clean audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobPreviewFeedbackApi(req, res, pathname) {
  if (!dashboardAuth(req, res)) return;

  const id = decodeURIComponent(pathname.slice('/api/jobs/'.length, -'/preview/feedback'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    await readJob(id);
    const payload = await readJsonBody(req);
    const rating = String(payload.rating || '').trim().slice(0, 40);
    const note = String(payload.note || '').trim().slice(0, 2000);
    if (!rating && !note) {
      sendJson(res, 400, { ok: false, error: 'rating or note is required' });
      return;
    }
    const preview = await readOptionalDataJson(path.relative(DATA_ROOT, previewMetaPathForId(id)));
    const feedback = await readPreviewFeedback(id);
    const entry = {
      id: crypto.randomUUID(),
      createdAt: new Date().toISOString(),
      rating,
      note,
      previewCreatedAt: preview?.createdAt,
      params: preview?.params || null,
      metrics: preview?.metrics || null
    };
    feedback.unshift(entry);
    const trimmed = feedback.slice(0, 50);
    await writePreviewFeedback(id, trimmed);
    sendJson(res, 201, { ok: true, feedback: trimmed });
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'job not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJob(req, res, pathname) {
  const id = decodeURIComponent(pathname.slice('/jobs/'.length));
  if (!isValidJobId(id)) {
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

async function handleAudio(req, res, url) {
  if (!ASR_FILE_TOKEN || !timingSafeEqualText(url.searchParams.get('token'), ASR_FILE_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid audio token' });
    return;
  }

  const recordingName = decodeURIComponent(url.pathname.slice('/audio/'.length));
  const asrCleanMatch = recordingName.match(/^([\w.-]+)\.clean-for-asr\.wav$/i);
  if (asrCleanMatch) {
    const id = asrCleanMatch[1];
    if (!isValidJobId(id)) {
      sendJson(res, 400, { ok: false, error: 'invalid job id' });
      return;
    }
    try {
      const cleanPath = asrCleanPathForId(id);
      const stat = await fsp.stat(cleanPath);
      res.writeHead(200, {
        'Content-Type': 'audio/wav',
        'Content-Length': stat.size,
        'Cache-Control': 'no-store'
      });
      fs.createReadStream(cleanPath).pipe(res);
    } catch (error) {
      if (error.code === 'ENOENT') {
        sendJson(res, 404, { ok: false, error: 'ASR clean audio not found' });
        return;
      }
      sendJson(res, 500, { ok: false, error: error.message });
    }
    return;
  }

  const normalized = normalizeRecordingName(recordingName);
  if (!normalized || normalized.recordingName !== recordingName) {
    sendJson(res, 400, { ok: false, error: 'invalid audio name' });
    return;
  }

  const uploadPath = path.join(UPLOAD_DIR, normalized.recordingName);
  try {
    const stat = await fsp.stat(uploadPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store'
    });
    fs.createReadStream(uploadPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleAsrAudio(req, res, url) {
  if (!ASR_FILE_TOKEN || !timingSafeEqualText(url.searchParams.get('token'), ASR_FILE_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid audio token' });
    return;
  }

  const id = decodeURIComponent(url.pathname.slice('/asr-audio/'.length));
  if (!isValidJobId(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }

  try {
    const stat = await fsp.stat(asrCleanPathForId(id));
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'no-store'
    });
    fs.createReadStream(asrCleanPathForId(id)).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'ASR clean audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleJobProcess(req, res, pathname) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }
  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const id = decodeURIComponent(pathname.slice('/jobs/'.length, -'/process'.length));
  if (!/^[\w.-]+$/.test(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }
  const job = await readJob(id);
  job.status = 'uploaded';
  job.phase = Math.max(2, Number(job.phase || 2));
  job.reprocessRequestedAt = new Date().toISOString();
  delete job.pendingReason;
  delete job.lastError;
  await writeJob(job);
  scheduleProcessJob(id);
  sendJson(res, 202, { ok: true, id, status: 'queued' });
}

async function handleJobResend(req, res, pathname) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }
  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const id = decodeURIComponent(pathname.slice('/jobs/'.length, -'/resend'.length));
  if (!/^[\w.-]+$/.test(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
    return;
  }
  if (!FLOMO_WEBHOOK_URL) {
    sendJson(res, 500, { ok: false, error: 'flomo is not configured' });
    return;
  }

  let job;
  try {
    job = await readJob(id);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'job not found' });
      return;
    }
    throw error;
  }
  let text = job.transcriptText || '';
  if (!text && job.transcriptPath) {
    text = await fsp.readFile(path.join(DATA_ROOT, job.transcriptPath), 'utf8');
  }
  text = text.trim();
  if (!text) {
    sendJson(res, 409, { ok: false, error: 'job has no transcript text to resend' });
    return;
  }

  const flomoPayload = await sendToFlomo(job, text);
  const transcriptMemoPath = transcriptMemoPathForId(id);
  await fsp.writeFile(transcriptMemoPath, `${flomoPayload.content}\n`, 'utf8');

  job = await readJob(id);
  job.status = 'done';
  job.phase = 4;
  job.transcriptMemoPath = path.relative(DATA_ROOT, transcriptMemoPath);
  job.memo = {
    title: flomoPayload.memo.title,
    hasTodos: flomoPayload.memo.todos.length > 0,
    hasIdeas: flomoPayload.memo.ideas.length > 0
  };
  job.flomo = {
    ...(job.flomo || {}),
    resentAt: new Date().toISOString(),
    memoSlug: flomoPayload.result?.memo?.slug || job.flomo?.memoSlug
  };
  delete job.pendingReason;
  delete job.lastError;
  await writeJob(job);

  sendJson(res, 200, {
    ok: true,
    id,
    status: 'resent',
    memo: job.memo,
    transcriptMemoPath: job.transcriptMemoPath
  });
}

async function route(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  const pathname = url.pathname;

  if (req.method === 'GET' && pathname === '/health') {
    await handleHealth(req, res);
    return;
  }
  if (req.method === 'GET' && pathname === '/dashboard') {
    sendHtml(res, 200, dashboardLabHtml());
    return;
  }
  if (req.method === 'GET' && pathname === '/dashboard/lab') {
    redirect(res, '/dashboard');
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/dashboard/jobs/')) {
    const id = decodeURIComponent(pathname.slice('/dashboard/jobs/'.length));
    redirect(res, isValidJobId(id) ? `/dashboard?job=${encodeURIComponent(id)}` : '/dashboard');
    return;
  }
  if (req.method === 'GET' && pathname === '/api/dashboard') {
    await handleDashboardApi(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname === '/api/preview-feedback') {
    await handlePreviewFeedbackApi(req, res, url);
    return;
  }
  if (req.method === 'POST' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/preview')) {
    await handleJobPreviewApi(req, res, pathname);
    return;
  }
  if (req.method === 'POST' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/asr-clean')) {
    await handleJobAsrCleanApi(req, res, pathname);
    return;
  }
  if (req.method === 'POST' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/preview/feedback')) {
    await handleJobPreviewFeedbackApi(req, res, pathname);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/asr-clean/audio')) {
    await handleJobAsrCleanAudioApi(req, res, pathname);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/preview/audio')) {
    await handleJobPreviewAudioApi(req, res, pathname);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/api/jobs/') && pathname.endsWith('/audio')) {
    await handleJobAudioApi(req, res, pathname);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/api/jobs/')) {
    await handleJobDetailApi(req, res, pathname);
    return;
  }
  if (req.method === 'POST' && pathname === '/upload') {
    await handleUpload(req, res);
    return;
  }
  if (req.method === 'GET' && pathname === '/jobs') {
    await handleJobsList(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/audio/')) {
    await handleAudio(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/asr-audio/')) {
    await handleAsrAudio(req, res, url);
    return;
  }
  if (req.method === 'POST' && pathname.startsWith('/jobs/') && pathname.endsWith('/process')) {
    await handleJobProcess(req, res, pathname);
    return;
  }
  if (req.method === 'POST' && pathname.startsWith('/jobs/') && pathname.endsWith('/resend')) {
    await handleJobResend(req, res, pathname);
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
