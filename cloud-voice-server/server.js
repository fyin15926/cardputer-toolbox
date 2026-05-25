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

  if (fs.existsSync(uploadPath) || fs.existsSync(jobPath)) {
    req.resume();
    scheduleProcessJob(jobId);
    sendJson(res, 200, { ok: true, id: jobId, duplicate: true });
    return;
  }

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
      recordedAt: rawRecordedAt.slice(0, 40),
      bytes,
      uploadPath: path.relative(DATA_ROOT, uploadPath),
      createdAt: startedAt,
      updatedAt: new Date().toISOString()
    };

    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');
    scheduleProcessJob(jobId);
    sendJson(res, 201, { ok: true, id: jobId });
  } catch (error) {
    out.destroy();
    await fsp.rm(uploadPath, { force: true }).catch(() => {});
    const statusCode = error.message === 'upload too large' ? 413 : 500;
    sendJson(res, statusCode, { ok: false, error: error.message });
  }
}

async function handleJobsList(req, res, url) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }
  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const rawLimit = parseInt(url.searchParams.get('limit') || '20', 10);
  const limit = Math.max(1, Math.min(Number.isFinite(rawLimit) ? rawLimit : 20, 100));
  const status = String(url.searchParams.get('status') || '').trim();
  if (status && !/^[\w.-]+$/.test(status)) {
    sendJson(res, 400, { ok: false, error: 'invalid status filter' });
    return;
  }

  try {
    const entries = await fsp.readdir(JOB_DIR, { withFileTypes: true });
    const jobs = [];
    let skipped = 0;

    await Promise.all(entries.map(async (entry) => {
      if (!entry.isFile() || !entry.name.endsWith('.json')) {
        return;
      }
      try {
        const raw = await fsp.readFile(path.join(JOB_DIR, entry.name), 'utf8');
        const job = JSON.parse(raw);
        if (!status || job.status === status) {
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

    sendJson(res, 200, {
      ok: true,
      count: Math.min(jobs.length, limit),
      total: jobs.length,
      skipped,
      jobs: jobs.slice(0, limit)
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
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
