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
const MAX_UPLOAD_BYTES = parseInt(process.env.MAX_UPLOAD_BYTES || `${25 * 1024 * 1024}`, 10);
const DASH_SCOPE_TRANSCRIPTION_URL = 'https://dashscope.aliyuncs.com/api/v1/services/audio/asr/transcription';
const DASH_SCOPE_TASK_URL = 'https://dashscope.aliyuncs.com/api/v1/tasks/';

const DATA_ROOT = process.env.DATA_ROOT
  ? path.resolve(process.env.DATA_ROOT)
  : __dirname;
const UPLOAD_DIR = path.join(DATA_ROOT, 'uploads');
const JOB_DIR = path.join(DATA_ROOT, 'jobs');
const TRANSCRIPT_DIR = path.join(DATA_ROOT, 'transcripts');
const TERMS_PATH = process.env.TERMS_PATH
  ? path.resolve(process.env.TERMS_PATH)
  : path.join(DATA_ROOT, 'terms.json');
const processingJobs = new Set();
const activeUploads = new Map();
const deviceStatuses = new Map();

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
    fsp.mkdir(TRANSCRIPT_DIR, { recursive: true })
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
        <table><thead><tr><th>录音</th><th>设备</th><th>进度</th><th>字节</th><th>开始时间</th></tr></thead><tbody id="uploads"></tbody></table>
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
        return '<tr><td>' + esc(u.recordingName) + '</td><td>' + esc(u.deviceId) + '</td><td><div class="progress"><div class="fill" style="width:' + pct + '%"></div></div> ' + pct + '%</td><td>' + fmtBytes(u.bytesReceived) + ' / ' + fmtBytes(u.totalBytes) + '</td><td>' + fmtTime(u.startedAt) + '</td></tr>';
      }).join('') : '<tr><td colspan="5" class="muted">当前没有正在接收的上传。</td></tr>';

      $('jobs').innerHTML = data.jobs.jobs.length ? data.jobs.jobs.map(j => {
        const note = j.lastError || j.pendingReason || (j.memo && j.memo.title) || '';
        const canProcess = String(j.status || '').includes('failed') || j.status === 'uploaded' || j.status === 'transcribed';
        const canResend = j.status === 'done' || j.status === 'transcribed';
        const actions = '<div class="actions">' +
          '<a class="button" href="/dashboard/jobs/' + encodeURIComponent(j.id) + '">查看</a>' +
          (canProcess ? '<button class="small" onclick="reprocessJob(\\'' + esc(j.id) + '\\')">重跑</button>' : '') +
          (canResend ? '<button class="small danger" onclick="resendJob(\\'' + esc(j.id) + '\\')">重发</button>' : '') +
          '</div>';
        return '<tr><td class="nowrap">' + esc(j.id) + '</td><td>' + statusPill(j.status) + '</td><td>' + esc(j.deviceId || '-') + '</td><td class="nowrap">' + fmtBytes(j.bytes) + '</td><td>' + esc(j.recordedAt || '-') + '</td><td class="nowrap">' + fmtTime(j.updatedAt || j.createdAt) + '</td><td>' + esc(note) + '</td><td>' + actions + '</td></tr>';
      }).join('') : '<tr><td colspan="8" class="muted">没有匹配任务。</td></tr>';
    }

    load();
    setInterval(load, 3000);
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
    input, button { height: 38px; border: 1px solid var(--line); background: var(--soft); color: var(--text); border-radius: 6px; padding: 0 10px; font: inherit; min-width: 0; }
    button { cursor: pointer; color: var(--ok); white-space: nowrap; }
    button:hover { border-color: var(--ok); }
    button.danger { color: var(--bad); }
    .top { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 12px; }
    .bar { display: grid; grid-template-columns: minmax(220px, 1fr) auto auto auto; gap: 8px; align-items: center; }
    .panel { border: 1px solid var(--line); background: var(--panel); border-radius: 8px; padding: 12px; margin-top: 14px; }
    .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }
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
    .timeline { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
    .step { border: 1px solid var(--line); border-radius: 8px; padding: 10px; background: #0c120f; min-height: 78px; }
    .step strong { display: block; margin-bottom: 6px; }
    pre { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font: 12px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace; background: #080d0a; border: 1px solid #17251d; border-radius: 6px; padding: 10px; max-height: 420px; overflow: auto; }
    audio { width: 100%; margin-top: 10px; }
    .hidden { display: none !important; }
    @media (max-width: 900px) { .grid, .timeline { grid-template-columns: repeat(2, minmax(0, 1fr)); } .bar { grid-template-columns: 1fr auto; } }
    @media (max-width: 560px) { .grid, .timeline { grid-template-columns: 1fr; } .top { align-items: flex-start; flex-direction: column; } .bar { grid-template-columns: 1fr 1fr; } #token { grid-column: 1 / -1; } }
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
    <section class="panel">
      <div class="grid">
        <div><div class="label">状态</div><div id="status" class="value">-</div></div>
        <div><div class="label">阶段</div><div id="phase" class="value">-</div></div>
        <div><div class="label">设备</div><div id="device" class="value">-</div></div>
        <div><div class="label">大小</div><div id="bytes" class="value">-</div></div>
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

    <section class="panel">
      <div class="section-title">
        <h2>原始录音</h2>
        <div class="actions"><button id="loadAudio">加载 WAV</button></div>
      </div>
      <div id="audioStatus" class="muted">需要 token 才会读取音频。</div>
      <audio id="audio" controls class="hidden"></audio>
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
      <h2>任务 JSON</h2>
      <pre id="raw">-</pre>
    </section>
  </main>
  <script>
    const $ = (id) => document.getElementById(id);
    const jobId = decodeURIComponent(location.pathname.split('/').pop() || '');
    const fmtTime = (v) => v ? new Date(v).toLocaleString() : '-';
    const fmtBytes = (n) => Number.isFinite(n) ? (n > 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.round(n / 1024) + ' KB') : '-';
    const esc = (v) => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const normalizeToken = (value) => String(value || '').trim().replace(/^UPLOAD_TOKEN\\s*=\\s*/i, '').replace(/^token\\s*=\\s*/i, '').replace(/^['"]|['"]$/g, '').trim();
    const tokenInput = $('token');
    tokenInput.value = localStorage.getItem('cardputerUploadToken') || '';
    $('title').textContent = jobId ? '录音详情 ' + jobId : '录音详情';
    $('save').onclick = () => { tokenInput.value = normalizeToken(tokenInput.value); localStorage.setItem('cardputerUploadToken', tokenInput.value); load(); };
    $('refresh').onclick = () => load();
    $('clear').onclick = () => { localStorage.removeItem('cardputerUploadToken'); tokenInput.value = ''; load(); tokenInput.focus(); };
    $('loadAudio').onclick = () => loadAudio();

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

    function render(data) {
      const job = data.job;
      $('status').innerHTML = statusPill(job.status);
      $('phase').textContent = job.phase ?? '-';
      $('device').textContent = job.deviceId || '-';
      $('bytes').textContent = fmtBytes(job.bytes);
      $('recording').textContent = job.recordingName || job.id || '';
      $('stamp').textContent = '更新 ' + fmtTime(data.time);
      const reason = job.lastError || job.pendingReason || '';
      $('errorPanel').classList.toggle('hidden', !reason);
      $('jobError').textContent = reason || '';
      $('transcript').textContent = data.transcriptText || '还没有转写文本。';
      $('memo').textContent = data.memoText || '还没有 flomo memo。';
      $('raw').textContent = JSON.stringify(job, null, 2);
      $('timeline').innerHTML = [
        step('上传', job.createdAt ? 'done' : 'waiting', fmtTime(job.createdAt), job.recordedAt || ''),
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

    const audioUrl = publicAudioUrl(job.recordingName);
    job.status = 'transcribing';
    job.phase = 3;
    job.audioUrl = audioUrl.replace(ASR_FILE_TOKEN, '***');
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
  if (!contentType.includes('audio/wav') && !contentType.includes('audio/x-wav')) {
    sendJson(res, 415, { ok: false, error: 'Content-Type must be audio/wav' });
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

  if (fs.existsSync(uploadPath) || fs.existsSync(jobPath)) {
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

  let bytes = 0;
  const out = fs.createWriteStream(uploadPath, { flags: 'wx' });
  activeUploads.set(jobId, uploadProgress);

  req.on('data', (chunk) => {
    bytes += chunk.length;
    uploadProgress.bytesReceived = bytes;
    uploadProgress.updatedAt = new Date().toISOString();
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
      recordedAt: rawRecordedAt.slice(0, 40),
      bytes,
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
    out.destroy();
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
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      job: redactJobForDashboard(job),
      transcriptText,
      memoText,
      files: {
        audio: Boolean(job.recordingName),
        transcript: Boolean(job.transcriptPath),
        memo: Boolean(memoText)
      }
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
    const uploadPath = path.join(UPLOAD_DIR, normalized.recordingName);
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
  await readJob(id);
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
    sendHtml(res, 200, dashboardHtml());
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/dashboard/jobs/')) {
    sendHtml(res, 200, dashboardJobHtml());
    return;
  }
  if (req.method === 'GET' && pathname === '/api/dashboard') {
    await handleDashboardApi(req, res, url);
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
