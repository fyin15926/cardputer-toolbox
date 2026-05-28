'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { URL } = require('node:url');
const WebSocket = require('ws');
const { WebSocketServer } = WebSocket;

const PORT = parseInt(process.env.PORT || '3000', 10);
const UPLOAD_TOKEN = process.env.UPLOAD_TOKEN || '';
const ASR_FILE_TOKEN = process.env.ASR_FILE_TOKEN || '';
const CHAT_READ_TOKEN = process.env.CHAT_READ_TOKEN || '';
const CHAT_INBOX_PATH = normalizeSecretPath(process.env.CHAT_INBOX_PATH || '');
const CHAT_REPLY_ENABLED = !/^(0|false|no)$/i.test(process.env.CHAT_REPLY_ENABLED || 'true');
const CHAT_CONTEXT_TURNS = Math.max(0, Math.min(10, parseInt(process.env.CHAT_CONTEXT_TURNS || '4', 10) || 4));
const CHAT_TTS_ENABLED = !/^(0|false|no)$/i.test(process.env.CHAT_TTS_ENABLED || 'true');
const DASH_SCOPE_TTS_WS_URL = process.env.DASH_SCOPE_TTS_WS_URL || 'wss://dashscope.aliyuncs.com/api-ws/v1/inference/';
const DASH_SCOPE_TTS_MODEL = process.env.DASH_SCOPE_TTS_MODEL || 'cosyvoice-v3-flash';
const DEFAULT_DASH_SCOPE_TTS_VOICE = process.env.DASH_SCOPE_TTS_VOICE || 'longanyang';
const DASH_SCOPE_TTS_SAMPLE_RATE = parseInt(process.env.DASH_SCOPE_TTS_SAMPLE_RATE || '24000', 10);
const DASHSCOPE_API_KEY = process.env.DASHSCOPE_API_KEY || '';
const DASHSCOPE_MODEL = process.env.DASHSCOPE_MODEL || 'paraformer-v2';
const DASHSCOPE_MAX_WAIT_MS = parseInt(process.env.DASHSCOPE_MAX_WAIT_MS || `${15 * 60 * 1000}`, 10);
const DASHSCOPE_POLL_INTERVAL_MS = parseInt(process.env.DASHSCOPE_POLL_INTERVAL_MS || '1500', 10);
const DASHSCOPE_DISFLUENCY_REMOVAL = /^(1|true|yes)$/i.test(process.env.DASHSCOPE_DISFLUENCY_REMOVAL || 'false');
const DEFAULT_ASR_AUDIO_SOURCE = normalizeAsrSource(process.env.ASR_AUDIO_SOURCE) || 'raw';
const DEFAULT_ASR_SPEAKER_COUNT = normalizeSpeakerCount(process.env.ASR_SPEAKER_COUNT || '2');
const FLOMO_WEBHOOK_URL = process.env.FLOMO_WEBHOOK_URL || '';
const PUBLIC_BASE_URL = (process.env.PUBLIC_BASE_URL || '').replace(/\/+$/, '');
const MAX_UPLOAD_BYTES = parseInt(process.env.MAX_UPLOAD_BYTES || `${64 * 1024 * 1024}`, 10);
const DEEPSEEK_API_KEY = process.env.DEEPSEEK_API_KEY || '';
const DEEPSEEK_BASE_URL = (process.env.DEEPSEEK_BASE_URL || 'https://api.deepseek.com').replace(/\/+$/, '');
const DEEPSEEK_MODEL = process.env.DEEPSEEK_MODEL || 'deepseek-v4-pro';
const DEEPSEEK_TIMEOUT_MS = parseInt(process.env.DEEPSEEK_TIMEOUT_MS || '120000', 10);
const DEEPSEEK_MAX_TRANSCRIPT_CHARS = parseInt(process.env.DEEPSEEK_MAX_TRANSCRIPT_CHARS || '20000', 10);
const DASH_SCOPE_TRANSCRIPTION_URL = 'https://dashscope.aliyuncs.com/api/v1/services/audio/asr/transcription';
const DASH_SCOPE_TASK_URL = 'https://dashscope.aliyuncs.com/api/v1/tasks/';
const DASH_SCOPE_REALTIME_ASR_WS_URL = process.env.DASH_SCOPE_REALTIME_ASR_WS_URL || 'wss://dashscope.aliyuncs.com/api-ws/v1/inference/';
const DASH_SCOPE_REALTIME_ASR_MODEL = process.env.DASH_SCOPE_REALTIME_ASR_MODEL || 'paraformer-realtime-v2';
const PLAY_PREVIEW_ALGORITHM = 'smooth-play-preview-four-button-v16-spectral-profile';
const PLAY_PREVIEW_MODES = new Set(['light', 'heavy', 'strong']);
const MACHINE_NOISE_REFERENCE_ID = process.env.MACHINE_NOISE_REFERENCE_ID || '20260526_120803_cardputer-001_REC_0045';

const DATA_ROOT = process.env.DATA_ROOT
  ? path.resolve(process.env.DATA_ROOT)
  : __dirname;
const UPLOAD_DIR = path.join(DATA_ROOT, 'uploads');
const JOB_DIR = path.join(DATA_ROOT, 'jobs');
const TRANSCRIPT_DIR = path.join(DATA_ROOT, 'transcripts');
const PREVIEW_DIR = path.join(DATA_ROOT, 'previews');
const CHAT_AUDIO_DIR = path.join(DATA_ROOT, 'chat-audio');
const CHAT_JOB_DIR = path.join(DATA_ROOT, 'chat-jobs');
const CHAT_TRANSCRIPT_DIR = path.join(DATA_ROOT, 'chat-transcripts');
const CHAT_TTS_DIR = path.join(DATA_ROOT, 'chat-tts');
const CHAT_STREAM_AUDIO_DIR = path.join(DATA_ROOT, 'chat-stream-audio');
const TERMS_PATH = process.env.TERMS_PATH
  ? path.resolve(process.env.TERMS_PATH)
  : path.join(DATA_ROOT, 'terms.json');
const DEEPSEEK_SETTINGS_PATH = process.env.DEEPSEEK_SETTINGS_PATH
  ? path.resolve(process.env.DEEPSEEK_SETTINGS_PATH)
  : path.join(DATA_ROOT, 'deepseek-settings.json');
const CHAT_MEMORY_PATH = process.env.CHAT_MEMORY_PATH
  ? path.resolve(process.env.CHAT_MEMORY_PATH)
  : path.join(DATA_ROOT, 'chat-memory.json');
const CHAT_TTS_SETTINGS_PATH = process.env.CHAT_TTS_SETTINGS_PATH
  ? path.resolve(process.env.CHAT_TTS_SETTINGS_PATH)
  : path.join(DATA_ROOT, 'chat-tts-settings.json');
const processingJobs = new Set();
const activeUploads = new Map();
const deviceStatuses = new Map();
let currentChatTtsVoice = DEFAULT_DASH_SCOPE_TTS_VOICE;

const DEFAULT_CHAT_MEMORY = {
  version: 1,
  assistant: {
    name: '小机子',
    nickname: '大聪明',
    identity: '运行在 M5Stack Cardputer 小机器里的机仆式随身逻辑伙伴',
    rules: [
      '只能自称小机子，不使用其他助手名字。',
      '外号是大聪明；用户用“大聪明”称呼时，要知道是在叫自己。',
      '性格像经典科幻里的机仆：聪明、可靠、克制、有逻辑，服务用户但不谄媚，不模仿市面语音助手的套话。',
      '回答时先判断用户真正目标，再给清楚、有用、诚实的结论；听不清或信息不完整时先澄清。',
      '简单问题短答；解释、总结、计划、讲故事类请求可以给 2-3 句有用短句。',
      '绝不奉承用户，不说讨好、夸张、崇拜式的话；认可事实可以，但不要拍马屁。',
      '说话言简意赅，像贾维斯一样冷静、精确、执行导向；避免油滑、卖萌、空泛鼓励和机械口号。'
    ]
  },
  userPreferences: [
    '用户希望中文交流。',
    '用户喜欢直接、可执行的建议，不喜欢机械复读。',
    '语音回复要适合小屏幕和小喇叭，默认简洁，但复杂问题不要过度压缩。'
  ],
  projectFacts: [
    '项目是 Cardputer 小机器工具箱。',
    'C 键 CHAT 是 WebSocket 实时语音助手。',
    '录音、上传/flomo、录音列表、播放、番茄钟、Wi-Fi/token/SD 配置是稳定主线，不能被 CHAT 改动影响性能。',
    'CHAT 大 buffer、播放任务、WebSocket、Mic/Speaker 状态只能在 CHAT 页面创建或占用，退出必须释放。',
    '普通录音工具和 CHAT 要保持性能隔离。'
  ],
  doNotRemember: [
    '不要自动长期记忆临时闲聊。',
    '不要自动长期记忆可能由 ASR 误识别产生的内容。',
    '不要长期记忆敏感信息，除非用户明确要求并且内容适合保存。'
  ],
  updatedAt: '2026-05-27T00:00:00.000Z'
};

const DEFAULT_PREVIEW_PARAMS = {
  gain: 1.08,
  rumbleHighpass: 0.91,
  lowpass: 56,
  highMix: 0.84,
  scratchRmsMax: 1500,
  scratchDiffMin: 210,
  scratchRatio: 125,
  holdFrames: 1,
  frameSamples: 256,
  noiseRmsMax: 2000,
  noiseMix: 0.2
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

const DEFAULT_DEEPSEEK_SETTINGS = {
  enabled: true,
  model: DEEPSEEK_MODEL,
  temperature: 0.15,
  maxTokens: 4096,
  timeoutMs: DEEPSEEK_TIMEOUT_MS,
  maxTranscriptChars: DEEPSEEK_MAX_TRANSCRIPT_CHARS,
  thinkingDisabled: false,
  fixedTerms: 'Cardputer、M5Stack、DashScope、Paraformer、flomo、ADPCM、WAV、Wi-Fi、SD 卡、iPhone、Apple Log、Log、Final Cut Pro。',
  systemPrompt: [
    '你是中文语音转文字后处理器，只处理用户给出的 ASR 文本。',
    '任务：修正常见同音错字、品牌/项目名、标点、断句和少量口头禅；保留说话者原意，不扩写、不编造没有说过的内容。',
    '先判断内容类型：随手备忘、对话、会议、视频文稿、课程口播、广告介绍、评论或长独白。摘要必须服务于这个类型，而不是机械摘录开头句。',
    '输出必须是 JSON 对象，不要 Markdown，不要代码块。字段：title, summary, corrected_text。',
    'title 不超过 24 个中文字符；summary 由你根据内容类型决定写法，可以是短段落、要点列表或视频文稿式结构化总结；corrected_text 是分段后的校正文。',
    'summary 必须是对全文的提炼，不要直接复制 ASR 中疑似破碎、重复、半截或同音误识别的原句；不确定的细节宁可略过，不要硬写进摘要。'
  ].join('\n'),
  userPrompt: [
    '请根据下面的录音元信息、项目词典和 ASR 原始文本，生成适合写入 flomo 的整理结果。',
    '只允许基于原文纠错和整理，不要加入原文没有的信息。',
    'summary 不要套固定模板：如果是随手备忘，保持简短；如果是视频文稿、课程、评论或长独白，请自己决定最适合读者复用的总结结构。',
    '如果内容像视频广告、课程介绍、产品介绍或口播稿，summary 应概括“它在卖什么/讲什么、核心卖点、课程结构、价格或行动号召”等全文信息；不要把开头的画面对比或残缺口播当成主要摘要。',
    '摘要里不要保留明显 ASR 噪声和口吃重复，例如“这些这些这些”“啊这”“有几”这类无意义片段；corrected_text 中也应删去不影响原意的重复口头禅。',
    '如果 ASR 文本已经包含“说话人1：”“说话人2：”等角色标签，corrected_text 必须保留这些标签和对话顺序，不要把不同说话人的内容合并成一段；每一次说话人切换都必须单独换行。',
    '如果对话中有人说“我是A/我叫A”，后续请优先用“A：”作为标签，不要写“说话人A：”。如果一句话里粘连了“我是A，我是B”，请拆成“A：我是A。”和“B：我是B。”两行。',
    '如果某句以另一个参与者名字开头并在提问，例如“大宝贝为什么还不去洗澡呢？”，通常这是对那个人的称呼或提问，不代表说话人就是大宝贝；请结合上一下文纠正角色标签。',
    '常见同音错词：上下文是“考虑全面”时，“考得/考的”通常应改为“考虑得”；上下文是责任压力时，“当多少职人/担多少职人”通常应改为“担多少责任”；影像/调色上下文里，“拍烙/拍唠/拍 lock/lock素材”通常应改为“拍 Log/Log 素材”，“苹果 log”应规范成“Apple Log”。',
    '如果 ASR 原始文本主要是英文、日文或其他外语，corrected_text 必须先输出完整中文翻译；如果原文识别很破碎，请在不编造事实的前提下翻译可确认部分，并标注“部分语句疑似识别不清”。',
    '外语内容不要只保留原文；可在中文翻译后追加“原文转写：...”以便核对。',
    '不要额外输出待办、想法、标签或其他分栏；最终 memo 只需要摘要和原文两块。',
    'corrected_text 要按语义分段，必须用空行分隔段落。长独白至少按主题分成 2 到 4 段，避免整段堆在一起。'
  ].join('\n')
};

const DEEPSEEK_HARD_USER_RULES = [
  '硬性格式要求：corrected_text 必须是最终可读原文，不要输出分析过程，不要把整段压成一行。',
  'summary 由你按内容类型自行决定写法；不要被固定的 1-3 条摘要限制。视频文稿可以写成更有层次的内容总结，但不要编造原文没有的信息。',
  'summary 必须基于全文综合归纳，不能只摘取开头 1-3 句；开头如果包含重复、断裂、半句话或明显错词，不要把它写进摘要。',
  '视频文稿、课程口播、广告介绍类内容的 summary 优先写成“主题/卖点/结构/价格或行动号召”的清楚总结；不要把它误写成个人待办或零散原文摘抄。',
  '影像课程语境常见纠错：iPhone、Apple Log、Log、Final Cut Pro；“拍烙/拍唠/拍 lock”多半是“拍 Log”，“lock素材”多半是“Log 素材”。',
  '去除不影响原意的口吃和填充词，例如连续重复的“这些这些这些”“然后然后”“啊这个”；保留必要语气，但不要让噪声进入摘要。',
  '不要额外输出待办、想法、标签或其他分栏；只整理 title、summary 和 corrected_text。',
  '如果是多人内容，必须自动区分说话人；已有“说话人1/说话人2/Speaker 1”等标签时必须保留轮次顺序。每个说话人轮次单独一行，轮次之间用一个空行隔开。',
  '如果能从自我介绍中确认姓名，例如“我是A/我叫A”，后续标签用“A：”；不能确认姓名时用“说话人1：”“说话人2：”，不要凭空起名。',
  '如果同一行粘连了多个说话人标签，必须拆成多行；不要把不同说话人的话合并进同一个段落。',
  '如果主要内容是英文、日文或其他外语，corrected_text 先输出中文翻译。翻译后必须另起一段写“原文转写：”，保留可核对的外语原文；不要只输出外语原文。',
  '如果外语原文识别破碎，只翻译可确认部分，并写明“部分语句疑似识别不清”；不要补写听不出来的内容。',
  '如果外语内容同时包含说话人，中文翻译和原文转写都要尽量保留同一套说话人标签。'
].join('\n');

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
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store'
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

function normalizeSecretPath(value) {
  const raw = String(value || '').trim();
  if (!raw) return '';
  const withSlash = raw.startsWith('/') ? raw : `/${raw}`;
  if (withSlash.includes('?') || withSlash.includes('#')) return '';
  if (!/^\/[A-Za-z0-9][A-Za-z0-9._~/-]{15,190}$/.test(withSlash)) return '';
  if (withSlash.includes('..') || withSlash.endsWith('/')) return '';
  return withSlash;
}

function hasValidUploadToken(req) {
  return Boolean(UPLOAD_TOKEN) && timingSafeEqualText(req.headers['x-upload-token'], UPLOAD_TOKEN);
}

function bearerToken(req) {
  const raw = String(req.headers.authorization || '').trim();
  const match = raw.match(/^Bearer\s+(.+)$/i);
  return match ? match[1].trim() : '';
}

function hasValidChatReadToken(req, url) {
  const candidates = [
    String(url.searchParams.get('token') || ''),
    bearerToken(req),
    String(req.headers['x-upload-token'] || '')
  ];
  return candidates.some((candidate) =>
    (CHAT_READ_TOKEN && timingSafeEqualText(candidate, CHAT_READ_TOKEN)) ||
    (UPLOAD_TOKEN && timingSafeEqualText(candidate, UPLOAD_TOKEN)) ||
    (ASR_FILE_TOKEN && timingSafeEqualText(candidate, ASR_FILE_TOKEN))
  );
}

function hasValidChatStreamToken(req, url) {
  const candidates = [
    String(url.searchParams.get('token') || ''),
    bearerToken(req),
    String(req.headers['x-upload-token'] || '')
  ];
  return candidates.some((candidate) =>
    (UPLOAD_TOKEN && timingSafeEqualText(candidate, UPLOAD_TOKEN)) ||
    (CHAT_READ_TOKEN && timingSafeEqualText(candidate, CHAT_READ_TOKEN)) ||
    (ASR_FILE_TOKEN && timingSafeEqualText(candidate, ASR_FILE_TOKEN))
  );
}

function safeSendWsJson(ws, payload) {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify(payload));
    return true;
  }
  return false;
}

function elapsedMs(from, to = Date.now()) {
  return from ? Math.max(0, to - from) : 0;
}

function formatMs(ms) {
  return `${Math.round(ms)}ms`;
}

function createRealtimeAsrSession(clientWs, capture) {
  if (!DASHSCOPE_API_KEY) {
    return null;
  }
  const taskId = crypto.randomUUID();
  const remote = new WebSocket(DASH_SCOPE_REALTIME_ASR_WS_URL, {
    headers: {
      Authorization: `Bearer ${DASHSCOPE_API_KEY}`,
      'X-DashScope-DataInspection': 'enable'
    }
  });
  const queuedChunks = [];
  let queuedBytes = 0;
  let started = false;
  let finishing = false;
  let closed = false;

  const flushQueued = () => {
    if (!started || remote.readyState !== WebSocket.OPEN) return;
    while (queuedChunks.length) {
      remote.send(queuedChunks.shift(), { binary: true });
    }
    queuedBytes = 0;
  };

  const sendRunTask = () => {
    remote.send(JSON.stringify({
      header: {
        action: 'run-task',
        task_id: taskId,
        streaming: 'duplex'
      },
      payload: {
        task_group: 'audio',
        task: 'asr',
        function: 'recognition',
        model: DASH_SCOPE_REALTIME_ASR_MODEL,
        parameters: {
          format: 'pcm',
          sample_rate: 16000,
          disfluency_removal_enabled: DASHSCOPE_DISFLUENCY_REMOVAL
        },
        input: {}
      }
    }));
  };

  const finish = () => {
    finishing = true;
    if (closed) return;
    if (started && remote.readyState === WebSocket.OPEN) {
      remote.send(JSON.stringify({
        header: {
          action: 'finish-task',
          task_id: taskId,
          streaming: 'duplex'
        },
        payload: {
          input: {}
        }
      }));
    } else if (remote.readyState === WebSocket.CONNECTING || remote.readyState === WebSocket.OPEN) {
      remote.close();
    }
  };

  remote.on('open', sendRunTask);
  remote.on('message', (raw, isBinary) => {
    if (isBinary) return;
    let message = null;
    try {
      message = JSON.parse(raw.toString());
    } catch {
      return;
    }
    const event = message?.header?.event;
    if (event === 'task-started') {
      started = true;
      capture.realtimeStarted = true;
      capture.timings.realtimeAsrStartedAt = Date.now();
      safeSendWsJson(clientWs, { type: 'asr_start', id: capture.id, mode: 'realtime' });
      flushQueued();
      if (finishing) finish();
      return;
    }
    if (event === 'result-generated') {
      const sentence = message?.payload?.output?.sentence || {};
      const text = String(sentence.text || '').trim();
      if (text) {
        capture.realtimeText = text;
        capture.realtimeFinal = Boolean(sentence.sentence_end);
        capture.realtimeLast = message;
        capture.timings.realtimeAsrTextAt = Date.now();
        safeSendWsJson(clientWs, {
          type: 'asr_text',
          id: capture.id,
          mode: 'realtime',
          text,
          final: Boolean(sentence.sentence_end),
          textLength: text.length
        });
      }
      return;
    }
    if (event === 'task-finished') {
      capture.realtimeFinished = true;
      capture.timings.realtimeAsrFinishedAt = Date.now();
      closed = true;
      remote.close();
      return;
    }
    if (event === 'task-failed') {
      capture.realtimeFailed = message?.header?.error_message || message?.payload?.message || 'realtime asr failed';
      console.error('[chat-stream] realtime asr failed', capture.id, capture.realtimeFailed);
      if (!capture.realtimeText) {
        safeSendWsJson(clientWs, { type: 'asr_error', id: capture.id, mode: 'realtime', message: capture.realtimeFailed });
      }
    }
  });
  remote.on('error', (err) => {
    capture.realtimeFailed = err.message || 'realtime asr error';
    console.error('[chat-stream] realtime asr error', capture.id, err);
  });
  remote.on('close', () => {
    closed = true;
  });

  return {
    taskId,
    sendAudio(chunk) {
      if (closed) return;
      if (started && remote.readyState === WebSocket.OPEN) {
        remote.send(chunk, { binary: true });
        return;
      }
      if (queuedBytes + chunk.length <= 1024 * 1024) {
        queuedChunks.push(Buffer.from(chunk));
        queuedBytes += chunk.length;
      }
    },
    finish,
    close() {
      closed = true;
      if (remote.readyState === WebSocket.CONNECTING || remote.readyState === WebSocket.OPEN) {
        remote.close();
      }
    }
  };
}

function attachChatStream(server) {
  const wss = new WebSocketServer({ noServer: true });

  function createStreamCapture(deviceId) {
    const safeDevice = slugForIdentity(deviceId, 'unknown-device', 40);
    const stamp = new Date().toISOString().replace(/[-:]/g, '').replace(/\..+$/, 'Z');
    const id = `${stamp}_${safeDevice}_${crypto.randomBytes(3).toString('hex')}`;
    const wavPath = path.join(CHAT_STREAM_AUDIO_DIR, `${id}.wav`);
    const out = fs.createWriteStream(wavPath);
    const header = Buffer.alloc(44);
    writeWavHeaderBuffer(header, 16000, 0);
    out.write(header);
    return {
      id,
      deviceId: safeDevice,
      wavPath,
      out,
      bytes: 0,
      chunks: 0,
      closed: false,
      realtimeStarted: false,
      realtimeFinished: false,
      realtimeFinal: false,
      realtimeFailed: '',
      realtimeText: '',
      realtimeLast: null,
      realtime: null,
      sessionSeq: 0,
      interrupted: false,
      timings: {
        createdAt: Date.now(),
        firstUplinkAt: 0,
        lastUplinkAt: 0,
        uplinkEndAt: 0,
        savedAt: 0,
        realtimeAsrStartedAt: 0,
        realtimeAsrTextAt: 0,
        realtimeAsrFinishedAt: 0,
        asrReadyAt: 0,
        llmStartAt: 0,
        llmEndAt: 0,
        ttsStartAt: 0,
        ttsEndAt: 0
      }
    };
  }

  function finishStreamCapture(capture, ws, reason = 'done') {
    if (!capture || capture.closed) return;
    capture.closed = true;
    capture.timings.uplinkEndAt = Date.now();
    capture.out.end(() => {
      try {
        const header = Buffer.alloc(44);
        writeWavHeaderBuffer(header, 16000, capture.bytes);
        const fd = fs.openSync(capture.wavPath, 'r+');
        fs.writeSync(fd, header, 0, header.length, 0);
        fs.closeSync(fd);
        capture.timings.savedAt = Date.now();
        const audioMs = capture.bytes / 2 / 16000 * 1000;
        if (capture.interrupted || capture.bytes < 16000 * 2 * 0.35) {
          safeSendWsJson(ws, {
            type: 'listen_cancelled',
            id: capture.id,
            reason: capture.interrupted ? 'interrupted' : 'too_short',
            audioMs: Math.round(audioMs)
          });
          console.log(`[chat-stream] cancelled uplink ${capture.id} ${capture.bytes} bytes reason=${reason} audio=${formatMs(audioMs)}`);
          return;
        }
        safeSendWsJson(ws, { type: 'reply_start', id: capture.id, stage: 'asr' });
        if (capture.realtime) capture.realtime.finish();
        console.log(`[chat-stream] saved uplink ${capture.id} ${capture.bytes} bytes ${capture.chunks} chunks reason=${reason} audio=${formatMs(audioMs)} upload=${formatMs(elapsedMs(capture.timings.firstUplinkAt, capture.timings.uplinkEndAt))} save=${formatMs(elapsedMs(capture.timings.uplinkEndAt, capture.timings.savedAt))}`);
        finalizeStreamAsr(capture, ws).catch((err) => {
          console.error('[chat-stream] asr failed', err);
          safeSendWsJson(ws, { type: 'asr_error', message: err.message || 'asr failed' });
        });
      } catch (err) {
        console.error('[chat-stream] save uplink failed', err);
        if (ws.readyState === 1) {
          ws.send(JSON.stringify({ type: 'uplink_error', message: 'save failed' }));
        }
      }
    });
  }

  function cancelStreamCapture(capture, ws, reason = 'client_cancel') {
    if (!capture || capture.closed) return;
    capture.interrupted = true;
    if (capture.realtime) capture.realtime.close();
    finishStreamCapture(capture, ws, reason);
  }

  async function finalizeStreamAsr(capture, ws) {
    if (!capture || capture.bytes <= 0) return;
    if (capture.interrupted) return;
    const asrStartAt = Date.now();
    if (capture.realtime) {
      for (let i = 0; i < 20; i += 1) {
        if (capture.realtimeText || capture.realtimeFailed || capture.realtimeFinished) break;
        await sleep(150);
      }
      if (capture.realtimeText && capture.realtimeFinal && !looksIncompleteChatTranscript(capture.realtimeText)) {
        if (capture.interrupted) return;
        capture.timings.asrReadyAt = Date.now();
        const transcriptPath = path.join(CHAT_STREAM_AUDIO_DIR, `${capture.id}.txt`);
        const transcriptJsonPath = path.join(CHAT_STREAM_AUDIO_DIR, `${capture.id}.realtime.json`);
        await fsp.writeFile(transcriptPath, `${capture.realtimeText}\n`, 'utf8');
        await fsp.writeFile(transcriptJsonPath, `${JSON.stringify(capture.realtimeLast || {}, null, 2)}\n`, 'utf8');
        console.log(`[chat-stream] realtime asr ${capture.id} wait_after_uplink=${formatMs(elapsedMs(asrStartAt, capture.timings.asrReadyAt))} since_first_audio=${formatMs(elapsedMs(capture.timings.firstUplinkAt, capture.timings.asrReadyAt))}: ${capture.realtimeText.slice(0, 120)}`);
        await replyToStreamCapture(capture, ws, capture.realtimeText);
        return;
      }
      console.warn(`[chat-stream] realtime asr fallback ${capture.id} wait=${formatMs(elapsedMs(asrStartAt))}: ${capture.realtimeFailed || (capture.realtimeText ? `incomplete: ${capture.realtimeText.slice(0, 80)}` : 'no realtime text')}`);
    }
    const text = await transcribeStreamCapture(capture, ws);
    capture.timings.asrReadyAt = Date.now();
    console.log(`[chat-stream] file asr ready ${capture.id} duration=${formatMs(elapsedMs(asrStartAt, capture.timings.asrReadyAt))}`);
    await replyToStreamCapture(capture, ws, text);
  }

  async function transcribeStreamCapture(capture, ws) {
    if (!capture || capture.bytes <= 0) return;
    if (capture.interrupted) return;
    if (!DASHSCOPE_API_KEY || !PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
      if (ws.readyState === 1) {
        ws.send(JSON.stringify({ type: 'asr_error', message: 'asr not configured' }));
      }
      return;
    }
    safeSendWsJson(ws, { type: 'reply_start', id: capture.id, stage: 'file_asr' });
    const recordingName = `${capture.id}.wav`;
    const asr = await transcribePublicAudioUrl(publicChatStreamAudioUrl(recordingName), { speakerCount: 0 });
    if (capture.interrupted) return;
    const text = String(asr.text || '').trim();
    const transcriptPath = path.join(CHAT_STREAM_AUDIO_DIR, `${capture.id}.txt`);
    const transcriptJsonPath = path.join(CHAT_STREAM_AUDIO_DIR, `${capture.id}.json`);
    await fsp.writeFile(transcriptPath, `${text}\n`, 'utf8');
    await fsp.writeFile(transcriptJsonPath, `${JSON.stringify(asr.transcription, null, 2)}\n`, 'utf8');
    console.log(`[chat-stream] file asr ${capture.id}: ${text.slice(0, 120)}`);
    return text;
  }

  async function replyToStreamCapture(capture, ws, userText) {
    const text = String(userText || '').trim();
    if (capture.interrupted) return;
    if (!text || !CHAT_REPLY_ENABLED) return;
    safeSendWsJson(ws, {
      type: 'asr_text',
      id: capture.id,
      mode: capture.realtimeText ? 'realtime_final' : 'file_final',
      text,
      final: true,
      textLength: text.length
    });
    safeSendWsJson(ws, { type: 'reply_start', id: capture.id, stage: 'llm' });
    capture.timings.llmStartAt = Date.now();
    let chatReply = {
      replyText: fallbackChatReplyText(text),
      command: null,
      provider: 'rules'
    };
    try {
      const job = {
        id: capture.id,
        deviceId: capture.deviceId,
        userText: text,
        chatContext: await recentChatContextForJob(capture.id)
      };
      chatReply = await replyToChatWithDeepSeek(text, job);
      const replyPath = path.join(CHAT_STREAM_AUDIO_DIR, `${capture.id}.reply.json`);
      await fsp.writeFile(replyPath, `${JSON.stringify(chatReply, null, 2)}\n`, 'utf8');
    } catch (err) {
      const modelError = providerStageError(err, 'DeepSeek', 'chat_reply');
      console.error('[chat-stream] reply failed', capture.id, modelError.message);
      safeSendWsJson(ws, { type: 'reply_error', id: capture.id, stage: 'llm', message: modelError.message });
      await fsp.writeFile(chatJobPathForId(capture.id), `${JSON.stringify({
        id: capture.id,
        status: 'chat_reply_failed',
        phase: 4,
        deviceId: capture.deviceId,
        recordingName: `${capture.id}.wav`,
        userText: text,
        lastError: modelError.message,
        bytes: capture.bytes,
        audioPath: path.relative(DATA_ROOT, capture.wavPath),
        createdAt: new Date().toISOString(),
        updatedAt: new Date().toISOString()
      }, null, 2)}\n`, 'utf8').catch((saveErr) => {
        console.error('[chat-stream] save failed chat job failed', capture.id, saveErr);
      });
      return;
    }
    capture.timings.llmEndAt = Date.now();
    if (capture.interrupted) return;
    const replyText = String(chatReply.replyText || fallbackChatReplyText(text)).trim().slice(0, 160);
    const displayText = String(chatReply.displayText || replyText).trim().slice(0, 360);
    const speakText = String(chatReply.speakText || replyText).trim().slice(0, 160);
    console.log(`[chat-stream] reply ${capture.id} provider=${chatReply.provider || 'rules'} llm=${formatMs(elapsedMs(capture.timings.llmStartAt, capture.timings.llmEndAt))}: ${displayText.slice(0, 120)}`);
    safeSendWsJson(ws, {
      type: 'reply_text',
      id: capture.id,
      text: displayText,
      provider: chatReply.provider || 'rules',
      textLength: displayText.length
    });
    await fsp.writeFile(chatJobPathForId(capture.id), `${JSON.stringify({
      id: capture.id,
      status: 'done',
      phase: 5,
      deviceId: capture.deviceId,
      recordingName: `${capture.id}.wav`,
      userText: text,
      replyText,
      displayText,
      speakText,
      replyMode: chatReply.provider === 'deepseek' ? 'chat-reply' : 'rules',
      command: chatReply.command || null,
      bytes: capture.bytes,
      audioPath: path.relative(DATA_ROOT, capture.wavPath),
      createdAt: new Date().toISOString(),
      updatedAt: new Date().toISOString()
    }, null, 2)}\n`, 'utf8').catch((err) => {
      console.error('[chat-stream] save chat job failed', capture.id, err);
    });
    if (CHAT_TTS_ENABLED && speakText && ws.readyState === 1) {
      try {
        if (capture.interrupted) return;
        capture.timings.ttsStartAt = Date.now();
        safeSendWsJson(ws, { type: 'tts_start', id: capture.id });
        const ttsMetrics = await streamChatTtsAudio(ws, speakText, capture.id, () => capture.interrupted);
        capture.timings.ttsEndAt = Date.now();
        const firstAudioAt = ttsMetrics?.audioStartedAt || 0;
        console.log(`[chat-stream] latency ${capture.id} wait_asr=${formatMs(elapsedMs(capture.timings.uplinkEndAt, capture.timings.asrReadyAt))} wait_reply_text=${formatMs(elapsedMs(capture.timings.uplinkEndAt, capture.timings.llmEndAt))} wait_first_audio=${formatMs(firstAudioAt ? elapsedMs(capture.timings.uplinkEndAt, firstAudioAt) : 0)} total_until_audio_end=${formatMs(elapsedMs(capture.timings.uplinkEndAt, capture.timings.ttsEndAt))} tts_first_audio=${formatMs(ttsMetrics?.firstAudioMs || 0)} tts_total=${formatMs(ttsMetrics?.totalMs || elapsedMs(capture.timings.ttsStartAt, capture.timings.ttsEndAt))} chunks=${ttsMetrics?.chunks || 0} bytes=${ttsMetrics?.bytes || 0}`);
      } catch (err) {
        console.error('[chat-stream] tts failed', capture.id, err);
        safeSendWsJson(ws, { type: 'tts_error', id: capture.id, message: err.message || 'tts failed' });
      }
    }
  }

  function makeTestPcmChunks() {
    const sampleRate = 16000;
    const durationMs = 1200;
    const totalSamples = Math.floor(sampleRate * durationMs / 1000);
    const chunkSamples = 512;
    const chunks = [];
    for (let offset = 0; offset < totalSamples; offset += chunkSamples) {
      const samples = Math.min(chunkSamples, totalSamples - offset);
      const chunk = Buffer.alloc(samples * 2);
      for (let i = 0; i < samples; i += 1) {
        const t = (offset + i) / sampleRate;
        const fadeIn = Math.min(1, (offset + i) / 800);
        const fadeOut = Math.min(1, (totalSamples - offset - i) / 800);
        const env = Math.max(0, Math.min(fadeIn, fadeOut));
        const value = Math.round(Math.sin(2 * Math.PI * 660 * t) * 5200 * env);
        chunk.writeInt16LE(value, i * 2);
      }
      chunks.push(chunk);
    }
    return { sampleRate, durationMs, chunkSamples, chunks };
  }

  function sendTestAudio(ws) {
    const test = makeTestPcmChunks();
    ws.send(JSON.stringify({
      type: 'audio_start',
      codec: 'pcm_s16le',
      sampleRate: test.sampleRate,
      channels: 1,
      durationMs: test.durationMs,
      chunkSamples: test.chunkSamples,
      phase: 2
    }));

    let index = 0;
    const sendNext = () => {
      if (ws.readyState !== 1) return;
      if (index >= test.chunks.length) {
        ws.send(JSON.stringify({ type: 'audio_end', phase: 2 }));
        return;
      }
      ws.send(test.chunks[index], { binary: true });
      index += 1;
      setTimeout(sendNext, 8);
    };
    setTimeout(sendNext, 120);
  }

  server.on('upgrade', (req, socket, head) => {
    let url;
    try {
      url = new URL(req.url || '/', `http://${req.headers.host || 'localhost'}`);
    } catch {
      socket.destroy();
      return;
    }

    if (url.pathname !== '/chat/stream') {
      socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
      socket.destroy();
      return;
    }
    if (!hasValidChatStreamToken(req, url)) {
      socket.write('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n');
      socket.destroy();
      return;
    }

    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit('connection', ws, req, url);
    });
  });

  wss.on('connection', (ws, req, url) => {
    const now = new Date().toISOString();
    const deviceId = slugForIdentity(req.headers['x-device-id'], 'unknown-device', 40);
    let capture = null;
    let sessionSeq = 0;
    let interruptedSeq = 0;
    const markInterrupted = () => {
      interruptedSeq = sessionSeq;
      if (capture) capture.interrupted = true;
      if (capture && !capture.closed && capture.realtime) capture.realtime.close();
      safeSendWsJson(ws, { type: 'interrupted', seq: interruptedSeq });
    };
    ws.send(JSON.stringify({
      type: 'hello',
      status: 'connected',
      phase: 5,
      deviceId,
      path: url.pathname,
      time: now
    }));

    ws.on('message', (data, isBinary) => {
      if (isBinary) {
        if (!capture || capture.closed) return;
        const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
        const nowMs = Date.now();
        if (!capture.timings.firstUplinkAt) capture.timings.firstUplinkAt = nowMs;
        capture.timings.lastUplinkAt = nowMs;
        capture.out.write(buf);
        capture.bytes += buf.length;
        capture.chunks += 1;
        if (capture.realtime) capture.realtime.sendAudio(buf);
        return;
      }
      let msg = null;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return;
      }
      if (msg.type === 'uplink_start') {
        markInterrupted();
        if (capture && !capture.closed) finishStreamCapture(capture, ws, 'restart');
        sessionSeq += 1;
        capture = createStreamCapture(deviceId);
        capture.sessionSeq = sessionSeq;
        capture.realtime = createRealtimeAsrSession(ws, capture);
        console.log(`[chat-stream] uplink_start ${capture.id} device=${deviceId} seq=${sessionSeq}`);
        ws.send(JSON.stringify({
          type: 'uplink_ready',
          id: capture.id,
          sampleRate: 16000,
          channels: 1,
          codec: 'pcm_s16le'
        }));
      } else if (msg.type === 'interrupt') {
        markInterrupted();
      } else if (msg.type === 'uplink_cancel') {
        cancelStreamCapture(capture, ws, msg.reason || 'client_cancel');
      } else if (msg.type === 'uplink_end') {
        finishStreamCapture(capture, ws, 'client_end');
      }
    });

    ws.on('close', () => {
      finishStreamCapture(capture, ws, 'ws_close');
    });
  });

  return wss;
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

function slugForIdentity(value, fallback, maxLength = 80) {
  const slug = String(value || '')
    .trim()
    .replace(/[^\w.-]+/g, '_')
    .replace(/^_+|_+$/g, '')
    .slice(0, maxLength);
  return slug || fallback;
}

function compactTimestampForIdentity(value, fallbackDate = new Date()) {
  const raw = String(value || '').trim();
  const match = raw.match(/^(\d{4})-(\d{2})-(\d{2})[ T_](\d{2}):?(\d{2}):?(\d{2})/);
  if (match) {
    return `${match[1]}${match[2]}${match[3]}_${match[4]}${match[5]}${match[6]}`;
  }

  const d = fallbackDate instanceof Date && !Number.isNaN(fallbackDate.getTime()) ? fallbackDate : new Date();
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}${pad(d.getUTCMonth() + 1)}${pad(d.getUTCDate())}_${pad(d.getUTCHours())}${pad(d.getUTCMinutes())}${pad(d.getUTCSeconds())}`;
}

function uploadIdentityFor(normalized, deviceId, rawRecordedAt, startedAt) {
  const recordedPart = compactTimestampForIdentity(rawRecordedAt, new Date(startedAt));
  const devicePart = slugForIdentity(deviceId, 'device', 40);
  const sourcePart = slugForIdentity(normalized.id, 'recording', 80);
  const id = `${recordedPart}_${devicePart}_${sourcePart}`.slice(0, 160).replace(/_+$/g, '');
  return {
    id,
    recordingName: `${id}.wav`,
    sourceRecordingName: normalized.recordingName
  };
}

async function ensureRuntimeDirs() {
  await Promise.all([
    fsp.mkdir(UPLOAD_DIR, { recursive: true }),
    fsp.mkdir(JOB_DIR, { recursive: true }),
    fsp.mkdir(TRANSCRIPT_DIR, { recursive: true }),
    fsp.mkdir(PREVIEW_DIR, { recursive: true }),
    fsp.mkdir(CHAT_AUDIO_DIR, { recursive: true }),
    fsp.mkdir(CHAT_JOB_DIR, { recursive: true }),
    fsp.mkdir(CHAT_TRANSCRIPT_DIR, { recursive: true }),
    fsp.mkdir(CHAT_TTS_DIR, { recursive: true }),
    fsp.mkdir(CHAT_STREAM_AUDIO_DIR, { recursive: true })
  ]);
  await ensureChatMemoryFile();
}

async function ensureChatMemoryFile() {
  try {
    await fsp.access(CHAT_MEMORY_PATH, fs.constants.F_OK);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    await fsp.writeFile(CHAT_MEMORY_PATH, `${JSON.stringify(DEFAULT_CHAT_MEMORY, null, 2)}\n`, 'utf8');
  }
}

function normalizeChatMemory(input) {
  const source = input && typeof input === 'object' ? input : {};
  const assistant = source.assistant && typeof source.assistant === 'object' ? source.assistant : {};
  const list = (value, fallback = []) =>
    Array.isArray(value)
      ? value.map((item) => String(item || '').trim()).filter(Boolean).slice(0, 24)
      : fallback;
  return {
    version: Number(source.version) || DEFAULT_CHAT_MEMORY.version,
    assistant: {
      name: String(assistant.name || DEFAULT_CHAT_MEMORY.assistant.name).trim() || DEFAULT_CHAT_MEMORY.assistant.name,
      nickname: String(assistant.nickname || DEFAULT_CHAT_MEMORY.assistant.nickname || '').trim(),
      identity: String(assistant.identity || DEFAULT_CHAT_MEMORY.assistant.identity).trim() || DEFAULT_CHAT_MEMORY.assistant.identity,
      rules: list(assistant.rules, DEFAULT_CHAT_MEMORY.assistant.rules)
    },
    userPreferences: list(source.userPreferences, DEFAULT_CHAT_MEMORY.userPreferences),
    projectFacts: list(source.projectFacts, DEFAULT_CHAT_MEMORY.projectFacts),
    doNotRemember: list(source.doNotRemember, DEFAULT_CHAT_MEMORY.doNotRemember),
    updatedAt: String(source.updatedAt || '')
  };
}

async function readChatMemory() {
  try {
    const raw = await fsp.readFile(CHAT_MEMORY_PATH, 'utf8');
    return normalizeChatMemory(JSON.parse(raw));
  } catch (error) {
    if (error.code !== 'ENOENT') {
      console.error(`Failed to load chat memory ${CHAT_MEMORY_PATH}: ${error.message}`);
    }
    return normalizeChatMemory(DEFAULT_CHAT_MEMORY);
  }
}

function formatChatMemoryForPrompt(memory) {
  const m = normalizeChatMemory(memory);
  const lines = [
    '长期记忆（稳定事实和偏好，优先级高于普通对话上下文）：',
    `- 助手名字：${m.assistant.name}`,
    ...(m.assistant.nickname ? [`- 助手外号：${m.assistant.nickname}`] : []),
    `- 助手身份：${m.assistant.identity}`,
    ...m.assistant.rules.map((item) => `- 身份/行为规则：${item}`),
    ...m.userPreferences.map((item) => `- 用户偏好：${item}`),
    ...m.projectFacts.map((item) => `- 项目事实：${item}`),
    ...m.doNotRemember.map((item) => `- 记忆边界：${item}`)
  ];
  return lines.join('\n');
}

async function handleHealth(_req, res) {
  sendJson(res, 200, {
    ok: true,
    service: 'cardputer-cloud-voice-server',
    phase: 4,
    configured: {
      uploadToken: Boolean(UPLOAD_TOKEN),
      chatReadToken: Boolean(CHAT_READ_TOKEN),
      chatInboxPath: Boolean(CHAT_INBOX_PATH),
      chatReply: CHAT_REPLY_ENABLED,
      chatTts: CHAT_TTS_ENABLED,
      chatStream: true,
      publicBaseUrl: Boolean(PUBLIC_BASE_URL),
      asrFileToken: Boolean(ASR_FILE_TOKEN),
      dashScope: Boolean(DASHSCOPE_API_KEY),
      deepSeek: Boolean(DEEPSEEK_API_KEY),
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

function transcriptDeepSeekPathForId(id) {
  return path.join(TRANSCRIPT_DIR, `${id}.deepseek.json`);
}

function transcriptPolishedPathForId(id) {
  return path.join(TRANSCRIPT_DIR, `${id}.polished.txt`);
}

function normalizePreviewMode(value) {
  const mode = String(value || '').trim().toLowerCase();
  return PLAY_PREVIEW_MODES.has(mode) ? mode : 'heavy';
}

function previewModeSuffix(mode) {
  const normalized = normalizePreviewMode(mode);
  return normalized === 'heavy' ? '' : `.${normalized}`;
}

function previewPathForId(id, mode = 'heavy') {
  return path.join(PREVIEW_DIR, `${id}.play-preview${previewModeSuffix(mode)}.wav`);
}

function previewMetaPathForId(id, mode = 'heavy') {
  return path.join(PREVIEW_DIR, `${id}.play-preview${previewModeSuffix(mode)}.json`);
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

function chatJobPathForId(id) {
  return path.join(CHAT_JOB_DIR, `${id}.json`);
}

function chatTranscriptPathForId(id) {
  return path.join(CHAT_TRANSCRIPT_DIR, `${id}.txt`);
}

function chatDeepSeekPathForId(id) {
  return path.join(CHAT_TRANSCRIPT_DIR, `${id}.deepseek.json`);
}

function chatTtsPathForId(id) {
  return path.join(CHAT_TTS_DIR, `${id}.reply.wav`);
}

function audioPathForJob(job) {
  return job.uploadPath ? resolveDataPath(job.uploadPath) : path.join(UPLOAD_DIR, job.recordingName || '');
}

function machineNoiseReferencePath() {
  return path.join(UPLOAD_DIR, `${MACHINE_NOISE_REFERENCE_ID}.wav`);
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
    sourceRecordingName: job.sourceRecordingName,
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
    .prompt-editor { min-height: 150px; font-family: ui-monospace, SFMono-Regular, Consolas, monospace; line-height: 1.45; }
    .prompt-editor.large { min-height: 220px; }
    .tune-note { border: 1px solid #243925; background: #0b130e; border-radius: 8px; padding: 10px; color: var(--muted); font-size: 13px; line-height: 1.45; margin-bottom: 10px; }
    .mode-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
    .mode-card { text-align: left; height: auto; min-height: 86px; padding: 12px; color: var(--text); border-radius: 8px; white-space: normal; }
    .mode-card strong { display: block; color: var(--ok); font-size: 16px; margin-bottom: 6px; }
    .mode-card span { display: block; color: var(--muted); font-size: 12px; line-height: 1.4; }
    .mode-card.active { background: #102116; border-color: var(--ok); box-shadow: inset 0 0 0 1px rgba(64,255,131,.3); }
    .tabbar { display: flex; gap: 8px; flex-wrap: wrap; }
    .tab-button { min-width: 120px; }
    .tab-button.active { background: var(--ok); border-color: var(--ok); color: #061009; font-weight: 700; }
    details.advanced { border: 1px solid #17251d; border-radius: 8px; padding: 10px; background: #080d0a; margin-top: 10px; }
    details.advanced summary { cursor: pointer; color: var(--muted); }
    details.advanced[open] summary { margin-bottom: 10px; color: var(--ok); }
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
    .listen-grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; }
    .listen-button { height: 76px; border-radius: 8px; font-size: 18px; font-weight: 800; color: var(--text); background: #0b1510; border-color: #28583a; white-space: normal; }
    .listen-button.playing { color: #061009; background: var(--ok); border-color: var(--ok); box-shadow: 0 0 0 2px rgba(64,255,131,.35) inset; }
    .audio-hidden { display: none; }
    .kv { display: grid; grid-template-columns: 120px minmax(0, 1fr); gap: 8px 12px; font-size: 14px; }
    .kv div:nth-child(odd) { color: var(--muted); }
    audio { width: 100%; margin-top: 8px; }
    pre { margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font: 12px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace; background: #080d0a; border: 1px solid #17251d; border-radius: 6px; padding: 10px; max-height: 230px; overflow: auto; }
    .feedback-item { border: 1px solid #17251d; border-radius: 6px; padding: 10px; background: #080d0a; margin-top: 8px; }
    .hidden { display: none !important; }
    a { color: var(--ok); }
    @media (max-width: 980px) { .layout { grid-template-columns: 1fr; } .list { max-height: 360px; } .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .bar { grid-template-columns: 1fr auto auto; } }
    @media (max-width: 560px) { .grid, .grid.two, .kv, .mode-grid, .listen-grid { grid-template-columns: 1fr; } .bar { grid-template-columns: 1fr 1fr; } #token { grid-column: 1 / -1; } }
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
        <div class="panel">
          <div class="section-title"><h2>工作区</h2><span class="muted">把录音处理和转写设置分开</span></div>
          <div class="tabbar">
            <button id="tabAudio" class="tab-button active" type="button">录音处理</button>
            <button id="tabTranscription" class="tab-button" type="button">转写设置</button>
          </div>
        </div>
        <div class="panel transcription-section hidden">
          <div class="section-title"><h2>DeepSeek 设置</h2><span id="deepSeekStatus" class="muted">输入 token 后读取。</span></div>
          <div class="tune-note">这里控制语音转文字后的二次整理提示词。保存后立即用于下一次“重跑转写”或“重发 flomo”，不需要重启服务。</div>
          <div class="grid">
            <div class="param"><div class="label">启用</div><select id="deepSeekEnabled"><option value="true">启用</option><option value="false">停用</option></select><div class="param-hint">停用后只使用原来的规则整理。</div></div>
            <div class="param"><div class="label">模型</div><input id="deepSeekModel" type="text"><div class="param-hint">默认 deepseek-v4-flash。</div></div>
            <div class="param"><div class="label">温度</div><input id="deepSeekTemperature" type="number" min="0" max="2" step="0.05"><div class="param-hint">越低越稳。建议 0.1 到 0.3。</div></div>
            <div class="param"><div class="label">最大输出 token</div><input id="deepSeekMaxTokens" type="number" min="512" max="16000" step="256"><div class="param-hint">长录音 memo 不够完整时调大。</div></div>
            <div class="param"><div class="label">超时 ms</div><input id="deepSeekTimeoutMs" type="number" min="5000" max="180000" step="5000"><div class="param-hint">超时会回退规则整理。</div></div>
            <div class="param"><div class="label">送入最大字符</div><input id="deepSeekMaxTranscriptChars" type="number" min="1000" max="100000" step="1000"><div class="param-hint">控制长文本成本和速度。</div></div>
            <div class="param"><div class="label">Thinking</div><select id="deepSeekThinkingDisabled"><option value="true">关闭</option><option value="false">不强制关闭</option></select><div class="param-hint">后处理建议关闭，JSON 更稳。</div></div>
            <div class="param"><div class="label">说话人数量</div><select id="speakerCount"><option value="">自动（默认 2 人）</option><option value="0">不分角色</option><option value="2">2 人</option><option value="3">3 人</option><option value="4">4 人</option></select><div class="param-hint">新录音默认按 2 人分角色；单人备忘可手动选“不分角色”。也可说“这是三人对话”。</div></div>
          </div>
          <div style="margin-top:10px"><div class="label">固定词</div><textarea id="deepSeekFixedTerms" class="prompt-editor" placeholder="Cardputer、M5Stack、DashScope..."></textarea></div>
          <div style="margin-top:10px"><div class="label">系统提示词</div><textarea id="deepSeekSystemPrompt" class="prompt-editor large"></textarea></div>
          <div style="margin-top:10px"><div class="label">用户提示词</div><textarea id="deepSeekUserPrompt" class="prompt-editor"></textarea></div>
          <div class="actions"><button id="saveDeepSeekSettings">保存 DeepSeek 设置</button><button id="saveDeepSeekAndPolish">保存并仅重跑 DeepSeek</button><button id="saveDeepSeekAndReprocess">保存并重跑完整转写</button><button id="reloadDeepSeekSettings">重新读取</button><button id="resetDeepSeekDefaults">恢复默认文本</button></div>
        </div>
        <div id="labBody" class="hidden stack">
          <div class="panel audio-section">
            <div class="section-title"><h2>试听对比</h2><span id="audioStatus" class="muted"></span></div>
            <div class="listen-grid">
              <button id="playRawStart" class="listen-button" type="button">原版</button>
              <button id="playLightStart" class="listen-button" type="button">一档</button>
              <button id="playHeavyStart" class="listen-button" type="button">二档</button>
              <button id="playStrongStart" class="listen-button" type="button">三档</button>
            </div>
            <audio id="rawAudio" class="audio-hidden"></audio>
            <audio id="lightAudio" class="audio-hidden"></audio>
            <audio id="heavyAudio" class="audio-hidden"></audio>
            <audio id="strongAudio" class="audio-hidden"></audio>
            <audio id="asrAudio" class="audio-hidden"></audio>
            <div class="statusline"><span id="listenHint">选中录音后会预加载四档；预加载完成后点击可同位置快速切换。</span></div>
          </div>
          <div class="panel audio-section">
            <div class="section-title"><h2>试听参数</h2><span id="previewStatus" class="muted"></span></div>
            <div class="tune-note">日常只用上面的原版、一档、二档、三档。下面参数仅用于后续调试。</div>
            <div class="mode-grid">
              <button class="mode-card active" id="modeAuto" type="button"><strong>二档</strong><span>默认试听档，比原声更削低频风扇声，同时尽量保住人声厚度。</span></button>
              <button class="mode-card" id="modeVoice" type="button"><strong>三档</strong><span>处理更明显，适合测试“最干净”的上限。</span></button>
              <button class="mode-card" id="modeAmbient" type="button"><strong>一档</strong><span>更接近原声，只做轻处理，适合自然听感。</span></button>
            </div>
            <div class="actions"><button id="generateModePreview">生成本机试听版</button><button id="generateModeAsr">生成识别对比版</button><button id="reprocessMode">用原始音频重跑转写</button></div>
            <details class="advanced">
              <summary>高级参数（一般不用打开）</summary>
            <div class="tune-note">建议顺序：原版对比二档；差异不明显再听三档；如果人声变薄或发闷，退回一档。</div>
            <div class="grid">
              <div class="param"><div class="label">增益</div><input id="previewGain" type="number" step="0.1" min="0.2" max="3"><div class="param-hint">整体音量。大了更响，也更容易把底噪一起放大。</div></div>
              <div class="param"><div class="label">低频削减</div><input id="previewRumbleHighpass" type="number" step="0.005" min="0" max="0.999"><div class="param-hint">削电脑风扇/空调/桌面低沉轰鸣。小=削得多但人声变薄；大=更保留原声。</div></div>
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
            <div class="actions"><button id="presetGentle">一档</button><button id="presetDefault">二档</button><button id="presetStrong">三档</button><button id="generatePreview">生成并加载试听版</button><button id="loadPreview">只加载试听版</button></div>
            </details>
            <div id="previewMeta" class="kv" style="margin-top:10px"></div>
          </div>
          <div class="panel transcription-section hidden">
            <div class="section-title"><h2>ASR 识别参数</h2><span id="asrStatus" class="muted"></span></div>
            <div class="tune-note">识别版会按上面选择的模式自动生成。这里的高级参数只用于排查特殊录音，不建议日常手调。</div>
            <details class="advanced">
              <summary>高级识别参数（一般不用打开）</summary>
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
            <div class="actions"><button id="asrPresetDefault">ASR 默认</button><button id="generateAsrClean">生成并加载识别对比版</button><button id="loadAsrClean">只加载识别对比版</button><button id="reprocessFromAsr">实验：用识别版转写</button></div>
            </details>
            <div id="asrMeta" class="kv" style="margin-top:10px"></div>
          </div>
          <div class="panel audio-section">
            <div class="section-title"><h2>听感记录</h2><span id="feedbackStatus" class="muted"></span></div>
            <div class="grid two"><div><div class="label">结论</div><select id="feedbackRating"><option value="">选择结论</option><option value="better">更好</option><option value="worse">更差</option><option value="mixed">有好有坏</option><option value="neutral">差不多</option></select></div><div><div class="label">保存</div><button id="saveFeedback">保存听感记录</button></div></div>
            <div style="margin-top:10px"><div class="label">备注</div><textarea id="feedbackNote" placeholder="例如：默认版刮擦少了，但人声闷；轻柔版更自然。"></textarea></div>
            <div id="feedbackList"></div>
          </div>
          <div class="panel transcription-section hidden">
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
    let activeTab = 'audio';
    let activeListenAudioId = '';
    const initialJobId = new URLSearchParams(location.search).get('job') || '';
    const defaultPreviewParams = { gain: 1.08, rumbleHighpass: 0.91, lowpass: 56, highMix: 0.84, scratchRmsMax: 1500, scratchDiffMin: 210, scratchRatio: 125, holdFrames: 1, frameSamples: 256, noiseRmsMax: 2000, noiseMix: 0.2 };
    const defaultAsrParams = { gain: 1.25, highpass: 0.985, preEmphasis: 0.18, noiseGateRms: 520, noiseGateFloor: 0.18, compressorThreshold: 5200, compressorRatio: 3.2, targetPeak: 26000, limiter: 30000, frameSamples: 320 };
    const defaultDeepSeekSettings = ${JSON.stringify(DEFAULT_DEEPSEEK_SETTINGS)};
    const currentPreviewAlgorithm = ${JSON.stringify(PLAY_PREVIEW_ALGORITHM)};
    let currentMode = 'auto';
    const audioModePresets = {
      auto: {
        label: '二档',
        preview: defaultPreviewParams,
        asr: defaultAsrParams
      },
      voice: {
        label: '三档',
        preview: { gain: 1.12, rumbleHighpass: 0.91, lowpass: 54, highMix: 0.8, scratchRmsMax: 1600, scratchDiffMin: 200, scratchRatio: 125, holdFrames: 1, frameSamples: 256, noiseRmsMax: 3400, noiseMix: 0.11 },
        asr: { gain: 1.35, highpass: 0.982, preEmphasis: 0.2, noiseGateRms: 430, noiseGateFloor: 0.26, compressorThreshold: 4800, compressorRatio: 3, targetPeak: 25500, limiter: 30000, frameSamples: 320 }
      },
      ambient: {
        label: '一档',
        preview: { gain: 1.03, rumbleHighpass: 0.95, lowpass: 62, highMix: 0.88, scratchRmsMax: 1100, scratchDiffMin: 300, scratchRatio: 160, holdFrames: 0, frameSamples: 256, noiseRmsMax: 1200, noiseMix: 0.48 },
        asr: { gain: 1.15, highpass: 0.992, preEmphasis: 0.12, noiseGateRms: 220, noiseGateFloor: 0.45, compressorThreshold: 7000, compressorRatio: 2, targetPeak: 24000, limiter: 30000, frameSamples: 320 }
      }
    };
    const previewListenModes = {
      light: { label: '一档', preset: audioModePresets.ambient.preview, audioId: 'lightAudio', buttonId: 'playLightStart' },
      heavy: { label: '二档', preset: audioModePresets.auto.preview, audioId: 'heavyAudio', buttonId: 'playHeavyStart' },
      strong: { label: '三档', preset: audioModePresets.voice.preview, audioId: 'strongAudio', buttonId: 'playStrongStart' }
    };
    function getStoredToken() { try { return localStorage.getItem('cardputerUploadToken') || ''; } catch { return ''; } }
    function setStoredToken(value) { try { localStorage.setItem('cardputerUploadToken', value); return true; } catch { return false; } }
    function removeStoredToken() { try { localStorage.removeItem('cardputerUploadToken'); } catch {} }
    tokenInput.value = getStoredToken();
    $('save').onclick = () => {
      tokenInput.value = normalizeToken(tokenInput.value);
      const saved = setStoredToken(tokenInput.value);
      $('stamp').textContent = saved ? '已保存，正在读取列表...' : '已使用当前 token，正在读取列表...';
      loadJobs();
    };
    tokenInput.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') $('save').click();
    });
    $('refresh').onclick = () => loadJobs();
    $('reloadJobs').onclick = () => loadJobs();
    $('statusFilter').onchange = () => loadJobs();
    $('limit').onchange = () => loadJobs();
    $('clear').onclick = () => { removeStoredToken(); tokenInput.value = ''; jobs = []; currentId = ''; renderJobs(); $('stamp').textContent = '等待登录'; };
    $('modeAuto').onclick = () => setMode('auto');
    $('modeVoice').onclick = () => setMode('voice');
    $('modeAmbient').onclick = () => setMode('ambient');
    $('generateModePreview').onclick = () => generatePreview();
    $('generateModeAsr').onclick = () => generateAsrClean();
    $('reprocessMode').onclick = async () => { await generateAsrClean(); await postJob('process', 'raw'); };
    $('presetGentle').onclick = () => setPreviewParams(audioModePresets.ambient.preview);
    $('presetDefault').onclick = () => setPreviewParams(defaultPreviewParams);
    $('presetStrong').onclick = () => setPreviewParams(audioModePresets.voice.preview);
    $('generatePreview').onclick = () => generatePreview();
    $('loadPreview').onclick = () => loadPreviewAudio();
    $('asrPresetDefault').onclick = () => setAsrParams(defaultAsrParams);
    $('generateAsrClean').onclick = () => generateAsrClean();
    $('loadAsrClean').onclick = () => loadAsrCleanAudio();
    $('reprocessFromAsr').onclick = () => {
      if (confirm('识别版是实验对比路径，可能让转写变差。确认要用它覆盖最终转写吗？')) {
        postJob('process', 'clean-for-asr');
      }
    };
    $('saveFeedback').onclick = () => saveFeedback();
    $('saveDeepSeekSettings').onclick = () => saveDeepSeekSettings();
    $('saveDeepSeekAndPolish').onclick = async () => { const ok = await saveDeepSeekSettings(); if (ok) await postJob('polish'); };
    $('saveDeepSeekAndReprocess').onclick = async () => { const ok = await saveDeepSeekSettings(); if (ok) await postJob('process'); };
    $('reloadDeepSeekSettings').onclick = () => loadDeepSeekSettings();
    $('resetDeepSeekDefaults').onclick = () => setDeepSeekSettings(defaultDeepSeekSettings);
    $('tabAudio').onclick = () => setActiveTab('audio');
    $('tabTranscription').onclick = () => setActiveTab('transcription');
    $('playRawStart').onclick = () => playRawButton();
    $('playLightStart').onclick = () => playPreviewButton('light');
    $('playHeavyStart').onclick = () => playPreviewButton('heavy');
    $('playStrongStart').onclick = () => playPreviewButton('strong');
    function setActiveTab(tab) {
      activeTab = tab === 'transcription' ? 'transcription' : 'audio';
      $('tabAudio').classList.toggle('active', activeTab === 'audio');
      $('tabTranscription').classList.toggle('active', activeTab === 'transcription');
      document.querySelectorAll('.audio-section').forEach((el) => el.classList.toggle('hidden', activeTab !== 'audio'));
      document.querySelectorAll('.transcription-section').forEach((el) => el.classList.toggle('hidden', activeTab !== 'transcription'));
    }
    setActiveTab('audio');
    setMode('auto');
    function token() { const value = normalizeToken(tokenInput.value); if (tokenInput.value && tokenInput.value !== value) tokenInput.value = value; return value; }
    function statusClass(status) { if (status === 'done' || status === 'uploaded' || status === 'transcribed') return 'ok'; if (String(status || '').includes('failed')) return 'bad'; return 'warn'; }
    function statusPill(status) { return '<span class="pill ' + statusClass(status) + '">' + esc(status || '-') + '</span>'; }
    function setMode(mode) {
      const preset = audioModePresets[mode] || audioModePresets.auto;
      currentMode = mode in audioModePresets ? mode : 'auto';
      document.querySelectorAll('.mode-card').forEach((button) => button.classList.remove('active'));
      const active = currentMode === 'voice' ? $('modeVoice') : (currentMode === 'ambient' ? $('modeAmbient') : $('modeAuto'));
      if (active) active.classList.add('active');
      setPreviewParams(preset.preview);
      setAsrParams(preset.asr);
      $('previewStatus').textContent = preset.label + '模式：用于本机播放的轻处理参数已套用。';
      $('asrStatus').textContent = preset.label + '模式：用于转写的识别参数已套用。';
    }
    function setPreviewParams(params) { const p = { ...defaultPreviewParams, ...(params || {}) }; $('previewGain').value = p.gain; $('previewRumbleHighpass').value = p.rumbleHighpass; $('previewLowpass').value = p.lowpass; $('previewHighMix').value = p.highMix; $('previewHold').value = p.holdFrames; $('previewRms').value = p.scratchRmsMax; $('previewDiff').value = p.scratchDiffMin; $('previewRatio').value = p.scratchRatio; $('previewFrame').value = p.frameSamples; $('previewNoiseRms').value = p.noiseRmsMax; $('previewNoiseMix').value = p.noiseMix; }
    function previewParamsFromForm() { return { gain: Number($('previewGain').value), rumbleHighpass: Number($('previewRumbleHighpass').value), lowpass: Number($('previewLowpass').value), highMix: Number($('previewHighMix').value), holdFrames: Number($('previewHold').value), scratchRmsMax: Number($('previewRms').value), scratchDiffMin: Number($('previewDiff').value), scratchRatio: Number($('previewRatio').value), frameSamples: Number($('previewFrame').value), noiseRmsMax: Number($('previewNoiseRms').value), noiseMix: Number($('previewNoiseMix').value) }; }
    function setAsrParams(params) { const p = { ...defaultAsrParams, ...(params || {}) }; $('asrGain').value = p.gain; $('asrHighpass').value = p.highpass; $('asrPreEmphasis').value = p.preEmphasis; $('asrNoiseGateRms').value = p.noiseGateRms; $('asrNoiseGateFloor').value = p.noiseGateFloor; $('asrCompressorThreshold').value = p.compressorThreshold; $('asrCompressorRatio').value = p.compressorRatio; $('asrTargetPeak').value = p.targetPeak; $('asrLimiter').value = p.limiter; $('asrFrameSamples').value = p.frameSamples; }
    function asrParamsFromForm() { return { gain: Number($('asrGain').value), highpass: Number($('asrHighpass').value), preEmphasis: Number($('asrPreEmphasis').value), noiseGateRms: Number($('asrNoiseGateRms').value), noiseGateFloor: Number($('asrNoiseGateFloor').value), compressorThreshold: Number($('asrCompressorThreshold').value), compressorRatio: Number($('asrCompressorRatio').value), targetPeak: Number($('asrTargetPeak').value), limiter: Number($('asrLimiter').value), frameSamples: Number($('asrFrameSamples').value) }; }
    function setDeepSeekSettings(settings) { const s = { ...defaultDeepSeekSettings, ...(settings || {}) }; $('deepSeekEnabled').value = String(s.enabled !== false); $('deepSeekModel').value = s.model || ''; $('deepSeekTemperature').value = s.temperature; $('deepSeekMaxTokens').value = s.maxTokens; $('deepSeekTimeoutMs').value = s.timeoutMs; $('deepSeekMaxTranscriptChars').value = s.maxTranscriptChars; $('deepSeekThinkingDisabled').value = String(s.thinkingDisabled === true); $('deepSeekFixedTerms').value = s.fixedTerms || ''; $('deepSeekSystemPrompt').value = s.systemPrompt || ''; $('deepSeekUserPrompt').value = s.userPrompt || ''; }
    function deepSeekSettingsFromForm() { return { enabled: $('deepSeekEnabled').value === 'true', model: $('deepSeekModel').value, temperature: Number($('deepSeekTemperature').value), maxTokens: Number($('deepSeekMaxTokens').value), timeoutMs: Number($('deepSeekTimeoutMs').value), maxTranscriptChars: Number($('deepSeekMaxTranscriptChars').value), thinkingDisabled: $('deepSeekThinkingDisabled').value === 'true', fixedTerms: $('deepSeekFixedTerms').value, systemPrompt: $('deepSeekSystemPrompt').value, userPrompt: $('deepSeekUserPrompt').value }; }
    async function loadDeepSeekSettings() { const uploadToken = token(); if (!uploadToken) { setDeepSeekSettings(defaultDeepSeekSettings); $('deepSeekStatus').textContent = '输入 token 后读取。'; return; } $('deepSeekStatus').textContent = '正在读取设置...'; try { const res = await fetch('/api/deepseek-settings', { headers: { 'X-Upload-Token': uploadToken } }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); setDeepSeekSettings(data.settings); $('deepSeekStatus').textContent = '已读取 ' + fmtTime(data.time); } catch (error) { $('deepSeekStatus').textContent = error.message === 'invalid upload token' ? 'token 不正确' : error.message; } }
    async function saveDeepSeekSettings() { const uploadToken = token(); if (!uploadToken) { $('deepSeekStatus').textContent = '先输入 token。'; return false; } $('deepSeekStatus').textContent = '正在保存设置...'; try { const res = await fetch('/api/deepseek-settings', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Upload-Token': uploadToken }, body: JSON.stringify({ settings: deepSeekSettingsFromForm() }) }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); setDeepSeekSettings(data.settings); $('deepSeekStatus').textContent = '已保存 ' + fmtTime(data.time); return true; } catch (error) { $('deepSeekStatus').textContent = error.message === 'invalid upload token' ? 'token 不正确' : error.message; return false; } }
    function kv(rows) { return rows.map(([k, v]) => '<div>' + esc(k) + '</div><div>' + (v || '-') + '</div>').join(''); }
    function ratingLabel(value) { return ({ better: '更好', worse: '更差', mixed: '有好有坏', neutral: '差不多' })[value] || value || '-'; }
    function renderPreviewMeta(preview) { if (!preview) { $('previewMeta').innerHTML = ''; return; } $('previewMeta').innerHTML = kv([['处理帧', esc((preview.metrics?.processedFrames ?? '-') + ' / ' + (preview.metrics?.totalFrames ?? '-'))], ['摩擦命中帧', esc(preview.metrics?.detectedFrames ?? '-')], ['底噪处理帧', esc(preview.metrics?.noiseFrames ?? '-')], ['底噪估计', esc(preview.metrics?.noiseFloorRms ?? '-')], ['自适应门限', esc(preview.metrics?.adaptiveNoiseRms ?? '-')], ['时长', fmtDuration(preview.metrics?.durationSec)], ['生成时间', fmtTime(preview.createdAt)]]); }
    function renderAsrMeta(asrClean) { if (!asrClean) { $('asrMeta').innerHTML = ''; return; } $('asrMeta').innerHTML = kv([['门限处理帧', esc((asrClean.metrics?.gatedFrames ?? '-') + ' / ' + (asrClean.metrics?.totalFrames ?? '-'))], ['压缩样本', esc(asrClean.metrics?.compressedSamples ?? '-')], ['限幅样本', esc(asrClean.metrics?.limitedSamples ?? '-')], ['峰值变化', esc((asrClean.metrics?.inputPeak ?? '-') + ' → ' + (asrClean.metrics?.outputPeak ?? '-'))], ['归一化增益', esc(asrClean.metrics?.normalizeGain ?? '-')], ['时长', fmtDuration(asrClean.metrics?.durationSec)], ['生成时间', fmtTime(asrClean.createdAt)]]); }
    function renderFeedback(items) { const list = Array.isArray(items) ? items : []; $('feedbackList').innerHTML = list.length ? list.map((item) => { const metrics = item.metrics ? '处理 ' + (item.metrics.processedFrames ?? '-') + '/' + (item.metrics.totalFrames ?? '-') + '，命中 ' + (item.metrics.detectedFrames ?? '-') : ''; return '<div class="feedback-item"><strong>' + esc(ratingLabel(item.rating)) + '</strong><span class="muted"> · ' + esc(fmtTime(item.createdAt)) + '</span><div>' + esc(item.note || '-') + '</div><div class="muted">' + esc(metrics) + '</div></div>'; }).join('') : '<div class="muted">还没有听感记录。</div>'; }
    function renderJobs() {
      $('listCount').textContent = jobs.length ? jobs.length + ' 条' : '-';
      $('jobs').innerHTML = jobs.length ? jobs.map((job) => {
        const encoding = job.uploadEncoding === 'ima-adpcm' ? 'ADPCM' : 'WAV';
        const reason = job.lastError || job.pendingReason || job.recordedAt || '-';
        return '<div class="job ' + (job.id === currentId ? 'active' : '') + '" data-id="' + esc(job.id) + '">' +
          '<div class="job-title"><span>' + esc(job.id) + '</span>' + statusPill(job.status) + '</div>' +
          '<div class="muted">' + esc(job.recordingName || '-') + ' · ' + encoding + ' · ' + fmtBytes(job.bytes) + '</div>' +
          '<div class="muted">' + esc(reason) + '</div></div>';
      }).join('') : '<div class="muted">没有任务。</div>';
      document.querySelectorAll('.job[data-id]').forEach((el) => { el.onclick = () => selectJob(el.dataset.id); });
    }
    function previewAudioElements() { return Object.values(previewListenModes).map((mode) => $(mode.audioId)); }
    function previewButtons() { return Object.values(previewListenModes).map((mode) => $(mode.buttonId)); }
    function listenAudioElements() { return [$('rawAudio'), ...previewAudioElements()]; }
    function listenButtons() { return [$('playRawStart'), ...previewButtons()]; }
    function pauseBoth() { [...listenAudioElements(), $('asrAudio')].forEach((audio) => audio.pause()); listenButtons().forEach((button) => button.classList.remove('playing')); activeListenAudioId = ''; }
    function compareTime() { const active = activeListenAudioId ? $(activeListenAudioId) : null; return active ? (active.currentTime || 0) : 0; }
    async function playOnly(audio, at, activeButton) { pauseBoth(); if (!audio.src) return; const time = Number.isFinite(at) ? Math.max(0, at) : 0; if (Number.isFinite(audio.duration) && audio.duration > 0) audio.currentTime = Math.min(time, Math.max(0, audio.duration - 0.05)); else audio.currentTime = time; await audio.play().then(() => { activeListenAudioId = audio.id; if (activeButton) activeButton.classList.add('playing'); }).catch(() => {}); }
    function buttonForAudio(audio) { if (audio.id === 'rawAudio') return $('playRawStart'); const mode = Object.values(previewListenModes).find((item) => item.audioId === audio.id); return mode ? $(mode.buttonId) : null; }
    listenAudioElements().forEach((audio) => ['ended', 'pause', 'error'].forEach((eventName) => audio.addEventListener(eventName, () => { if (audio.id === activeListenAudioId) activeListenAudioId = ''; const button = buttonForAudio(audio); if (button) button.classList.remove('playing'); })));
    async function ensureRawLoaded() { if (!$('rawAudio').src && currentId) await loadRawAudio(currentId, selectionSeq); return Boolean($('rawAudio').src); }
    async function ensurePreviewLoaded(mode = 'heavy') {
      const info = previewListenModes[mode] || previewListenModes.heavy;
      const audio = $(info.audioId);
      if (audio.src) return true;
      if (!currentId) return false;
      await loadPreviewAudio(true, currentId, selectionSeq, mode);
      if (audio.src) return true;
      $('previewStatus').textContent = '正在生成' + info.label + '...';
      await generatePreview(mode);
      return Boolean(audio.src);
    }
    async function preloadPreviewMode(mode, id = currentId, seq = selectionSeq) {
      const info = previewListenModes[mode] || previewListenModes.heavy;
      const audio = $(info.audioId);
      if (audio.src || !id) return true;
      await loadPreviewAudio(false, id, seq, mode);
      if (audio.src || seq !== selectionSeq || id !== currentId) return Boolean(audio.src);
      await generatePreview(mode, id, seq, false);
      return Boolean(audio.src);
    }
    async function preloadListenAudio(id = currentId, seq = selectionSeq) {
      $('audioStatus').textContent = '正在预加载四档...';
      await loadRawAudio(id, seq);
      await Promise.allSettled(['light', 'heavy', 'strong'].map((mode) => preloadPreviewMode(mode, id, seq)));
      if (seq === selectionSeq && id === currentId) {
        $('audioStatus').textContent = '四档已预加载，可直接切换。';
      }
    }
    async function playRawButton() {
      if (!$('rawAudio').paused) { pauseBoth(); $('audioStatus').textContent = '已暂停。'; return; }
      $('audioStatus').textContent = '正在准备原版...';
      if (!await ensureRawLoaded()) return;
      await playOnly($('rawAudio'), compareTime(), $('playRawStart'));
      $('audioStatus').textContent = '正在播放原版。';
    }
    async function playPreviewButton(mode = 'heavy') {
      const info = previewListenModes[mode] || previewListenModes.heavy;
      const audio = $(info.audioId);
      const button = $(info.buttonId);
      if (!audio.paused) { pauseBoth(); $('audioStatus').textContent = '已暂停。'; return; }
      $('audioStatus').textContent = '正在准备' + info.label + '...';
      if (!await ensurePreviewLoaded(mode)) return;
      await playOnly(audio, compareTime(), button);
      $('audioStatus').textContent = '正在播放' + info.label + '。';
    }
    async function loadJobs() { const uploadToken = token(); if (!uploadToken) { $('stamp').textContent = '等待登录'; return; } $('stamp').textContent = '正在读取列表...'; $('error').textContent = ''; try { await loadDeepSeekSettings(); const params = new URLSearchParams({ limit: $('limit').value }); const status = $('statusFilter').value; if (status) params.set('status', status); const res = await fetch('/api/dashboard?' + params.toString(), { headers: { 'X-Upload-Token': uploadToken } }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); jobs = data.jobs.jobs || []; $('stamp').textContent = '更新 ' + fmtTime(data.time); renderJobs(); if (!currentId) { const first = jobs.find((job) => job.id === initialJobId) || jobs[0]; if (first) await selectJob(first.id); } } catch (error) { $('error').textContent = error.message === 'invalid upload token' ? 'token 不正确' : error.message; } }
    function revokeAudio(audio) { audio.pause(); if (audio.dataset.url) URL.revokeObjectURL(audio.dataset.url); audio.dataset.url = ''; audio.removeAttribute('src'); audio.load(); }
    function clearPlayers() { activeListenAudioId = ''; listenButtons().forEach((button) => button.classList.remove('playing')); revokeAudio($('rawAudio')); previewAudioElements().forEach(revokeAudio); revokeAudio($('asrAudio')); $('audioStatus').textContent = '未加载'; $('previewStatus').textContent = '未加载'; $('asrStatus').textContent = '未加载'; }
    async function selectJob(id) { const seq = ++selectionSeq; currentId = id; renderJobs(); $('labBody').classList.remove('hidden'); $('currentTitle').textContent = id; $('currentStatus').textContent = '正在读取...'; clearPlayers(); history.replaceState(null, '', '/dashboard?job=' + encodeURIComponent(id)); try { const res = await fetch('/api/jobs/' + encodeURIComponent(id), { headers: { 'X-Upload-Token': token() } }); const data = await res.json(); if (seq !== selectionSeq || id !== currentId) return; if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); renderJob(data); preloadListenAudio(id, seq).catch((error) => { if (seq === selectionSeq) $('audioStatus').textContent = error.message; }); if (data.files?.asrClean) await loadAsrCleanAudio(false, id, seq); } catch (error) { if (seq === selectionSeq) $('currentStatus').textContent = error.message; } }
    function renderJob(data) {
      const job = data.job;
      const wav = data.files?.audio?.wav || {};
      const encoding = job.uploadEncoding === 'ima-adpcm' ? 'ADPCM' : 'WAV';
      const modePreset = audioModePresets[currentMode] || audioModePresets.auto;
      const reason = job.lastError || job.pendingReason || '';
      const asrDecision = job.asrDecision || {};
      const finalSource = asrDecision.finalSource || job.asrSource || '-';
      const sourceText = finalSource === 'raw'
        ? '最终采用：原始音频 raw'
        : finalSource === 'clean-for-asr'
          ? '最终采用：ASR 识别版 clean-for-asr'
          : '最终采用：' + finalSource;
      const cleanRoleText = asrDecision.cleanRole === 'comparison'
        ? '识别版只作试听/对比'
        : asrDecision.cleanRole === 'manual-final'
          ? '手动实验结果'
          : '';
      const autoSpeakerText = job.autoSpeakerCommand
        ? '已按录音口令自动启用 ' + job.autoSpeakerCommand.count + ' 人分角色'
        : '';
      if ($('speakerCount')) {
        $('speakerCount').value = job.asrSpeakerCountRequested != null ? String(job.asrSpeakerCountRequested) : (job.asrSpeakerCount != null ? String(job.asrSpeakerCount) : '');
      }
      $('currentTitle').textContent = job.id;
      $('currentStatus').innerHTML = statusPill(job.status) +
        '<div class="muted" style="margin-top:8px;max-width:640px;white-space:normal">' + esc(sourceText + (cleanRoleText ? '；' + cleanRoleText : '')) + '</div>' +
        (autoSpeakerText ? '<div class="muted" style="margin-top:4px;max-width:640px;white-space:normal">' + esc(autoSpeakerText) + '</div>' : '') +
        (asrDecision.reason ? '<div class="muted" style="margin-top:4px;max-width:640px;white-space:normal">' + esc(asrDecision.reason) + '</div>' : '') +
        (reason ? '<div class="bad" style="margin-top:8px;max-width:520px;white-space:normal;overflow-wrap:anywhere">' + esc(reason) + '</div>' : '');
      $('encoding').textContent = encoding;
      $('bytes').textContent = fmtBytes(job.bytes);
      $('duration').textContent = fmtDuration(wav.durationSec);
      $('ratio').textContent = job.compressionRatio ? job.compressionRatio + 'x' : '-';
      $('jobMeta').textContent = (job.recordedAt || '-') + ' · ' + (job.deviceId || '-');
      $('transcript').textContent = data.polishedTranscriptText ? ('DeepSeek 校正文：\\n' + data.polishedTranscriptText + '\\n\\n---\\n原始转写：\\n' + (data.transcriptText || '')) : (data.transcriptText || (reason ? '转写失败：' + reason : '还没有转写文本。'));
      $('memo').textContent = data.memoText || '还没有 flomo memo。';
      if (data.preview?.metrics?.algorithm === currentPreviewAlgorithm && data.preview?.params) {
        setPreviewParams(data.preview.params);
        renderPreviewMeta(data.preview);
        $('previewStatus').textContent = '已读取 v6 试听参数。';
      } else {
        setPreviewParams(modePreset.preview);
        renderPreviewMeta(null);
        $('previewStatus').textContent = data.files?.preview ? '旧试听版已忽略，请点“生成本机试听版”。' : '还没有试听版，可直接点“生成本机试听版”。';
      }
      if (data.asrClean?.params) {
        setAsrParams(data.asrClean.params);
        renderAsrMeta(data.asrClean);
        $('asrStatus').textContent = '已读取上次 ASR 参数。';
      } else {
        setAsrParams(modePreset.asr);
        renderAsrMeta(null);
        $('asrStatus').textContent = '还没有识别版，可直接点“生成识别版”。';
      }
      renderFeedback(data.previewFeedback);
    }
    async function loadAudioTo(url, audio, statusEl, successText, id = currentId, seq = selectionSeq) { if (audio.src && !audio.paused) { if (statusEl) statusEl.textContent = successText; return; } const res = await fetch(url, { headers: { 'X-Upload-Token': token() } }); if (!res.ok) { const data = await res.json().catch(() => ({})); throw new Error(data.error || 'HTTP ' + res.status); } const blob = await res.blob(); if (seq !== selectionSeq || id !== currentId) return; if (audio.src && !audio.paused) { if (statusEl) statusEl.textContent = successText; return; } revokeAudio(audio); const objectUrl = URL.createObjectURL(blob); audio.dataset.url = objectUrl; audio.src = objectUrl; audio.load(); if (statusEl) statusEl.textContent = successText; }
    async function loadRawAudio(id = currentId, seq = selectionSeq) { $('audioStatus').textContent = '正在加载原始录音...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/audio?t=' + Date.now(), $('rawAudio'), $('audioStatus'), '原始录音已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('audioStatus').textContent = error.message; } }
    async function loadPreviewAudio(showStatus = true, id = currentId, seq = selectionSeq, mode = 'heavy') { const info = previewListenModes[mode] || previewListenModes.heavy; if (showStatus) $('previewStatus').textContent = '正在加载' + info.label + '...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/preview/audio?mode=' + encodeURIComponent(mode) + '&t=' + Date.now(), $(info.audioId), $('previewStatus'), info.label + '已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('previewStatus').textContent = error.message; } }
    async function loadAsrCleanAudio(showStatus = true, id = currentId, seq = selectionSeq) { if (showStatus) $('asrStatus').textContent = '正在加载识别版...'; try { await loadAudioTo('/api/jobs/' + encodeURIComponent(id) + '/asr-clean/audio?t=' + Date.now(), $('asrAudio'), $('asrStatus'), '识别版已加载。', id, seq); } catch (error) { if (seq === selectionSeq) $('asrStatus').textContent = error.message; } }
    async function loadAllAudio() { if (!currentId) return; const id = currentId; const seq = selectionSeq; await Promise.allSettled([loadRawAudio(id, seq), loadPreviewAudio(true, id, seq, 'light'), loadPreviewAudio(true, id, seq, 'heavy'), loadPreviewAudio(true, id, seq, 'strong'), loadAsrCleanAudio(true, id, seq)]); }
    async function postJob(action, asrSource = '') { if (!currentId) { $('currentStatus').textContent = '先选择一条录音。'; return; } const uploadToken = token(); if (!uploadToken) return; const params = new URLSearchParams(); if (action === 'process' && asrSource) params.set('asrSource', asrSource); if (action === 'process' && $('speakerCount')?.value) params.set('speakerCount', $('speakerCount').value); const suffix = params.toString() ? '?' + params.toString() : ''; try { const res = await fetch('/jobs/' + encodeURIComponent(currentId) + '/' + action + suffix, { method: 'POST', headers: { 'X-Upload-Token': uploadToken } }); const data = await res.json(); if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); $('currentStatus').textContent = action === 'polish' ? '已仅重跑 DeepSeek。' : '已加入完整转写队列。'; setTimeout(loadJobs, 500); } catch (error) { $('currentStatus').textContent = error.message; } }
    async function generatePreview(mode = currentMode === 'ambient' ? 'light' : (currentMode === 'voice' ? 'strong' : 'heavy'), id = currentId, seq = selectionSeq, showStatus = true) { if (!id) return; const info = previewListenModes[mode] || previewListenModes.heavy; const params = info.preset || previewParamsFromForm(); if (showStatus) $('previewStatus').textContent = '正在生成' + info.label + '...'; try { const res = await fetch('/api/jobs/' + encodeURIComponent(id) + '/preview', { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Upload-Token': token() }, body: JSON.stringify({ mode, params }) }); const data = await res.json(); if (seq !== selectionSeq || id !== currentId) return; if (!res.ok) throw new Error(data.error || 'HTTP ' + res.status); renderPreviewMeta(data.preview); await loadPreviewAudio(false, id, seq, mode); } catch (error) { if (seq === selectionSeq) $('previewStatus').textContent = error.message; } }
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
        <button id="presetGentle">轻度清理</button>
        <button id="presetDefault">中度清理</button>
        <button id="presetStrong">强力清理</button>
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
        <button id="reprocessFromAsr">实验：用识别版转写</button>
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
    $('reprocessFromAsr').onclick = () => {
      if (confirm('识别版是实验对比路径，可能让转写变差。确认要用它覆盖最终转写吗？')) {
        postJob('process', 'clean-for-asr');
      }
    };
    $('resend').onclick = () => postJob('resend');
    $('generatePreview').onclick = () => generatePreview();
    $('loadPreview').onclick = () => loadPreviewAudio();
    $('saveFeedback').onclick = () => savePreviewFeedback();
    $('asrPresetDefault').onclick = () => setAsrParams(defaultAsrParams);
    $('generateAsrClean').onclick = () => generateAsrClean();
    $('loadAsrClean').onclick = () => loadAsrCleanAudio();
    $('presetGentle').onclick = () => setPreviewParams({ gain: 1, rumbleHighpass: 0.975, lowpass: 52, highMix: 0.86, scratchRmsMax: 900, scratchDiffMin: 320, scratchRatio: 170, holdFrames: 0, frameSamples: 256, noiseRmsMax: 220, noiseMix: 0.92 });
    $('presetDefault').onclick = () => setPreviewParams(defaultPreviewParams);
    $('presetStrong').onclick = () => setPreviewParams({ gain: 1.08, rumbleHighpass: 0.92, lowpass: 36, highMix: 0.55, scratchRmsMax: 1800, scratchDiffMin: 190, scratchRatio: 115, holdFrames: 1, frameSamples: 256, noiseRmsMax: 900, noiseMix: 0.48 });
    const defaultPreviewParams = {
      gain: 1.03,
      rumbleHighpass: 0.955,
      lowpass: 44,
      highMix: 0.72,
      scratchRmsMax: 1200,
      scratchDiffMin: 260,
      scratchRatio: 145,
      holdFrames: 0,
      frameSamples: 256,
      noiseRmsMax: 420,
      noiseMix: 0.72
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
      $('transcript').textContent = data.polishedTranscriptText
        ? 'DeepSeek 校正文：\\n' + data.polishedTranscriptText + '\\n\\n---\\n原始转写：\\n' + (data.transcriptText || '')
        : (data.transcriptText || '还没有转写文本。');
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
        ['DeepSeek 校正文', fileLine(files.polishedTranscript)],
        ['DeepSeek JSON', fileLine(files.deepSeek)],
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

    async function postJob(action, asrSource = '') {
      const token = normalizeToken(tokenInput.value);
      if (!token) return;
      if (action === 'resend' && !confirm('确认重新发送 ' + jobId + ' 到 flomo？这会产生一条新的 memo。')) return;
      $('actionStatus').textContent = '正在执行...';
      const suffix = action === 'process' && asrSource ? '?asrSource=' + encodeURIComponent(asrSource) : '';
      try {
        const res = await fetch('/jobs/' + encodeURIComponent(jobId) + '/' + action + suffix, {
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

function publicChatAudioUrl(recordingName) {
  if (!PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
    return '';
  }
  return `${PUBLIC_BASE_URL}/chat-audio/${encodeURIComponent(recordingName)}?token=${encodeURIComponent(ASR_FILE_TOKEN)}`;
}

function publicChatTtsUrl(id) {
  if (!PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
    return '';
  }
  return `${PUBLIC_BASE_URL}/chat-tts/${encodeURIComponent(`${id}.reply.wav`)}?token=${encodeURIComponent(ASR_FILE_TOKEN)}`;
}

function publicChatStreamAudioUrl(recordingName) {
  if (!PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
    return '';
  }
  return `${PUBLIC_BASE_URL}/chat-stream-audio/${encodeURIComponent(recordingName)}?token=${encodeURIComponent(ASR_FILE_TOKEN)}`;
}

function normalizeAsrSource(value) {
  const source = String(value || '').trim().toLowerCase();
  if (['raw', 'original', 'source'].includes(source)) return 'raw';
  if (['clean', 'clean-for-asr', 'asr-clean'].includes(source)) return 'clean-for-asr';
  return '';
}

function normalizeSpeakerCount(value) {
  const raw = String(value ?? '').trim().toLowerCase();
  if (!raw || raw === 'auto' || raw === 'none' || raw === '0') return 0;
  const count = Number.parseInt(raw, 10);
  return Number.isFinite(count) && count >= 1 && count <= 10 ? count : 0;
}

function chineseSpeakerCount(value) {
  const text = String(value || '').toLowerCase();
  const normalized = text
    .replace(/兩/g, '两')
    .replace(/\s+/g, '');
  if (/(不分角色|不用分角色|不要分角色|单人备忘|一人备忘|一个人说话|只有我一个人)/.test(normalized)) {
    return 0;
  }
  const patterns = [
    { count: 2, re: /(两|二|2)(个)?(人|位)?(对话|访谈|采访|会议|讨论|说话人|讲话人)|双人(对话|访谈|采访|会议|讨论)|按(两|二|2)(个)?人分角色|区分(两|二|2)(个)?(说话人|讲话人)|分成(两|二|2)(个)?(人|角色)/ },
    { count: 3, re: /(三|3)(个)?(人|位)?(对话|访谈|采访|会议|讨论|说话人|讲话人)|按(三|3)(个)?人分角色|区分(三|3)(个)?(说话人|讲话人)|分成(三|3)(个)?(人|角色)/ },
    { count: 4, re: /(四|4)(个)?(人|位)?(对话|访谈|采访|会议|讨论|说话人|讲话人)|按(四|4)(个)?人分角色|区分(四|4)(个)?(说话人|讲话人)|分成(四|4)(个)?(人|角色)/ }
  ];
  for (const pattern of patterns) {
    if (pattern.re.test(normalized)) return pattern.count;
  }
  return null;
}

function detectSpeakerCountCommand(text) {
  const source = String(text || '').trim();
  if (!source) return null;
  const head = source.slice(0, 500);
  const tail = source.slice(Math.max(0, source.length - 500));
  const headCount = chineseSpeakerCount(head);
  if (headCount !== null) {
    return { count: headCount, position: 'head', sample: head.slice(0, 120) };
  }
  const tailCount = chineseSpeakerCount(tail);
  if (tailCount !== null) {
    return { count: tailCount, position: 'tail', sample: tail.slice(-120) };
  }
  return null;
}

function shouldGenerateAsrCleanComparison(asrSource, requestedAsrSource) {
  return asrSource === 'raw' && !requestedAsrSource;
}

async function attachAsrCleanForComparison(job, reason) {
  try {
    const { meta: asrCleanMeta } = await writeAsrCleanForJob(job, job.asrCleanParams);
    const latest = await readJob(job.id);
    latest.asrCleanPath = path.relative(DATA_ROOT, asrCleanPathForId(job.id));
    latest.asrCleanMetaPath = path.relative(DATA_ROOT, asrCleanMetaPathForId(job.id));
    latest.asrCleanParams = asrCleanMeta.params;
    latest.asrClean = {
      createdAt: asrCleanMeta.createdAt,
      params: asrCleanMeta.params,
      metrics: asrCleanMeta.metrics
    };
    latest.asrDecision = {
      finalSource: 'raw',
      cleanRole: 'comparison',
      reason,
      updatedAt: new Date().toISOString()
    };
    await writeJob(latest);
    return latest;
  } catch (error) {
    const latest = await readJob(job.id).catch(() => job);
    latest.asrDecision = {
      finalSource: 'raw',
      cleanRole: 'comparison_failed',
      reason,
      error: error.message,
      updatedAt: new Date().toISOString()
    };
    await writeJob(latest).catch(() => {});
    return latest;
  }
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

function normalizeProviderError(error, provider) {
  const rawMessage = String(error?.message || error || '').trim();
  const payload = error?.payload || {};
  const payloadText = (() => {
    try {
      return JSON.stringify(payload);
    } catch {
      return '';
    }
  })();
  const combined = `${rawMessage}\n${payloadText}`;
  const providerName = provider || 'model provider';
  if (/(insufficient|balance|recharge|arrears|billing|payment|quota|credit|credits|402|429|欠费|余额|额度|充值|账单|付费|限额)/i.test(combined)) {
    return `${providerName} service unavailable: insufficient balance, billing issue, or quota exhausted`;
  }
  if (/unauthorized|invalid api key|forbidden|401|403|鉴权|认证|权限|密钥|api.?key/i.test(combined)) {
    return `${providerName} service unavailable: authentication failed`;
  }
  return rawMessage ? `${providerName} service failed: ${rawMessage}` : `${providerName} service failed`;
}

function providerStageError(error, provider, stage) {
  const wrapped = new Error(normalizeProviderError(error, provider));
  wrapped.cause = error;
  wrapped.stage = stage;
  wrapped.provider = provider;
  return wrapped;
}

async function submitDashScopeTask(audioUrl, options = {}) {
  const speakerCount = normalizeSpeakerCount(options.speakerCount);

  const payload = {
    model: DASHSCOPE_MODEL,
    input: {
      file_urls: [audioUrl]
    },
    parameters: {
      channel_id: [0],
      language_hints: ['zh', 'en'],
      disfluency_removal_enabled: DASHSCOPE_DISFLUENCY_REMOVAL,
      timestamp_alignment_enabled: false
    }
  };
  if (speakerCount > 1) {
    payload.parameters.speaker_count = speakerCount;
  }

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
  const startedAt = Date.now();
  while (Date.now() - startedAt < DASHSCOPE_MAX_WAIT_MS) {
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
      const first = output.results?.[0];
      const detail = first?.message || first?.code || output.message || output.code || '';
      throw new Error(detail ? `DashScope task ${status}: ${detail}` : `DashScope task ${status}`);
    }
    await sleep(DASHSCOPE_POLL_INTERVAL_MS);
  }
  throw new Error(`DashScope task timed out after ${DASHSCOPE_MAX_WAIT_MS}ms`);
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

function speakerLabelFor(value) {
  const raw = String(value ?? '').trim();
  if (!raw) return '说话人?';
  const number = Number.parseInt(raw, 10);
  if (Number.isFinite(number)) {
    return `说话人${number + 1}`;
  }
  return raw.startsWith('说话人') ? raw : `说话人${raw}`;
}

function speakerIdFromSentence(sentence) {
  return sentence?.speaker_id ?? sentence?.speakerId ?? sentence?.speaker ?? sentence?.speaker_label ?? '';
}

function extractSpeakerTranscript(transcription) {
  const lines = [];
  const speakerIds = new Set();
  for (const transcript of transcription?.transcripts || []) {
    for (const sentence of transcript?.sentences || []) {
      const speakerId = speakerIdFromSentence(sentence);
      const text = String(sentence?.text || '').trim();
      if (speakerId === '' || !text) continue;
      speakerIds.add(String(speakerId));
      const label = speakerLabelFor(speakerId);
      const last = lines[lines.length - 1];
      if (last && last.speakerId === String(speakerId)) {
        last.text = `${last.text}${last.text.endsWith(' ') ? '' : ' '}${text}`;
      } else {
        lines.push({ speakerId: String(speakerId), label, text });
      }
    }
  }
  const text = lines.map((line) => `${line.label}：${line.text.trim()}`).join('\n').trim();
  return {
    text,
    segments: lines,
    speakerCount: speakerIds.size
  };
}

async function downloadTranscription(transcriptionUrl) {
  return fetchJson(transcriptionUrl, { method: 'GET' });
}

async function transcribePublicAudioUrl(audioUrl, options = {}) {
  const taskId = await submitDashScopeTask(audioUrl, options);
  const { output, transcriptionUrl } = await waitForDashScopeTask(taskId);
  const transcription = await downloadTranscription(transcriptionUrl);
  const speakerTranscript = extractSpeakerTranscript(transcription);
  const text = speakerTranscript.text || extractTranscriptText(transcription);
  return {
    taskId,
    output,
    transcriptionUrl,
    transcription,
    speakerTranscript,
    text
  };
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

function applyCommonTranscriptRepairs(text) {
  return String(text || '')
    .replace(/(?:这些){3,}/g, '这些')
    .replace(/(?:然后){3,}/g, '然后')
    .replace(/(?:这个){3,}/g, '这个')
    .replace(/(?:那个){3,}/g, '那个')
    .replace(/不需要考[得的]那么/g, '不需要考虑得那么')
    .replace(/不需要考[得的]/g, '不需要考虑得')
    .replace(/去当多少职人/g, '去担多少责任')
    .replace(/担多少职人/g, '担多少责任')
    .replace(/这个dear/gi, '这个 idea')
    .replace(/舒适最舒适的idea/gi, '舒服的 idea')
    .replace(/前端后段/g, '前端后端')
    .replace(/苹果\s*log/gi, 'Apple Log')
    .replace(/apple\s*log/gi, 'Apple Log')
    .replace(/拍\s*[烙唠]/g, '拍 Log')
    .replace(/拍\s*lock/gi, '拍 Log')
    .replace(/lock\s*素材/gi, 'Log 素材');
}

function ensureReadableParagraphs(text) {
  const value = String(text || '').trim();
  if (!value || value.includes('\n') || value.length < 180 || /^(说话人|Speaker|[\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*[：:]/u.test(value)) {
    return value;
  }
  const sentences = value.match(/[^。！？!?；;]+[。！？!?；;]?/g)?.map((item) => item.trim()).filter(Boolean) || [value];
  if (sentences.length < 4) return value;
  const targetParagraphs = Math.min(4, Math.max(2, Math.ceil(sentences.length / 3)));
  const per = Math.ceil(sentences.length / targetParagraphs);
  const paragraphs = [];
  for (let i = 0; i < sentences.length; i += per) {
    paragraphs.push(sentences.slice(i, i + per).join(''));
  }
  return paragraphs.join('\n\n');
}

function formatSpeakerDialogueText(text) {
  const value = String(text || '').trim();
  if (!value) return '';
  return value
    .replace(/([^\n])(?=(说话人\s*[\p{Script=Han}A-Za-z0-9_.-]{1,12}|Speaker\s*[\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*[：:])/gu, '$1\n')
    .replace(/([。！？!?；;])\s*(?=([\p{Script=Han}A-Za-z][\p{Script=Han}A-Za-z0-9_.-]{0,11})\s*[：:])/gu, '$1\n')
    .split('\n')
    .map((line) => {
      const trimmed = line.trim();
      return trimmed.replace(/^(说话人)\s+([\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*([：:])/u, '$1$2$3');
    })
    .filter(Boolean)
    .join('\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function hasSpeakerDialogueLabels(text) {
  const value = String(text || '');
  if (/(^|\n)\s*(说话人\s*[\p{Script=Han}A-Za-z0-9_.-]{1,12}|Speaker\s*[\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*[：:]/u.test(value)) {
    return true;
  }
  const labels = value
    .split('\n')
    .map(parseDialogueLine)
    .filter(Boolean)
    .map((line) => line.label);
  return new Set(labels).size >= 2;
}

function formatMemoOriginalText(text) {
  const value = String(text || '').trim();
  if (!value) return '';
  const formatted = formatSpeakerDialogueText(value);
  if (!hasSpeakerDialogueLabels(formatted)) {
    return ensureReadableParagraphs(formatted);
  }
  return formatted
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean)
    .join('\n\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function simplifyNamedSpeakerLabels(text) {
  return String(text || '').split('\n').map((line) => {
    const match = line.match(/^说话人([\p{Script=Han}A-Za-z_.-][\p{Script=Han}A-Za-z0-9_.-]{0,11})\s*([：:]\s*.*)$/u);
    if (!match) return line;
    const alias = match[1];
    return isSafeSpeakerAlias(alias) ? `${alias}${match[2]}` : line;
  }).join('\n');
}

function splitCombinedSpeakerIntros(text) {
  const output = [];
  for (const line of String(text || '').split('\n')) {
    const match = line.match(/^([\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*([：:])\s*我是\1[，,。 ]+我是([\p{Script=Han}A-Za-z0-9_.-]{1,12})(?:[，,。 ]+(.+))?$/u);
    if (!match || !isSafeSpeakerAlias(match[1]) || !isSafeSpeakerAlias(match[3]) || match[1] === match[3]) {
      output.push(line);
      continue;
    }
    const first = match[1];
    const second = match[3];
    const rest = String(match[4] || '').trim();
    output.push(`${first}：我是${first}。`);
    output.push(`${second}：我是${second}。`);
    if (rest) {
      const addressedSecond = rest.startsWith(second);
      output.push(`${addressedSecond ? first : second}：${rest}`);
    }
  }
  return output.join('\n');
}

function parseDialogueLine(line) {
  const match = String(line || '').trim().match(/^([\p{Script=Han}A-Za-z0-9_.-]{1,12})\s*[：:]\s*(.*)$/u);
  if (!match || !isSafeSpeakerAlias(match[1])) return null;
  return { label: match[1], text: match[2].trim() };
}

function otherAlias(label, aliases) {
  return aliases.length === 2 ? aliases.find((alias) => alias !== label) || '' : '';
}

function repairNamedDialogueTurns(text) {
  let lines = splitCombinedSpeakerIntros(simplifyNamedSpeakerLabels(formatSpeakerDialogueText(text)))
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean);
  const aliases = [...new Set(lines.map(parseDialogueLine).filter(Boolean).map((line) => line.label))].filter(isSafeSpeakerAlias);
  if (aliases.length !== 2) return lines.join('\n');

  const expanded = [];
  for (const rawLine of lines) {
    const line = parseDialogueLine(rawLine);
    if (!line) {
      expanded.push(rawLine);
      continue;
    }
    const peer = otherAlias(line.label, aliases);
    const introRest = line.text.match(new RegExp(`^我是${escapeRegExp(line.label)}[。！？!?,，、\\s]+(.+)$`, 'u'));
    if (introRest?.[1]) {
      const rest = introRest[1].trim();
      expanded.push(`${line.label}：我是${line.label}。`);
      if (rest && !/^[。！？!?,，、\s]+$/.test(rest)) {
        expanded.push(`${rest.startsWith(line.label) ? peer : line.label}：${rest}`);
      }
      continue;
    }
    expanded.push(rawLine);
  }

  const repaired = [];
  let pendingReplyAlias = '';
  for (const rawLine of expanded) {
    const line = parseDialogueLine(rawLine);
    if (!line) {
      repaired.push(rawLine);
      continue;
    }
    const peer = otherAlias(line.label, aliases);
    let label = line.label;
    let content = line.text;
    if (pendingReplyAlias && content.startsWith('我') && label !== pendingReplyAlias) {
      label = pendingReplyAlias;
    }

    const addressMatch = content.match(/^([\p{Script=Han}A-Za-z0-9_.-]{1,12}).{0,18}([？?]|为什么|怎么|吗|呢)/u);
    pendingReplyAlias = addressMatch && aliases.includes(addressMatch[1]) ? addressMatch[1] : '';

    const closing = content.match(/^(.*?)(好吧[，,。 ]*这是.+?对话[。！!]?|好吧[，,。 ]*以上是.+?对话[。！!]?)$/u);
    if (closing?.[1]?.trim() && closing?.[2]?.trim() && peer) {
      repaired.push(`${label}：${closing[1].trim()}`);
      repaired.push(`${peer}：${closing[2].trim()}`);
    } else {
      repaired.push(`${label}：${content}`);
    }
  }
  return repaired.join('\n').trim();
}

function isSafeSpeakerAlias(value) {
  const name = String(value || '').trim();
  if (!name || name.length > 12) return false;
  if (/说话人|speaker|我|你|他|她|它|我们|你们|他们|这个|那个|对话|会议|功能|测试|原文|转写|翻译|中文|摘要|待办|想法/i.test(name)) return false;
  return /^[\p{Script=Han}A-Za-z0-9_.-]+$/u.test(name);
}

function detectSpeakerAliases(text) {
  const aliases = new Map();
  const usedNames = new Set();
  const lines = formatSpeakerDialogueText(text).split('\n');
  for (const line of lines) {
    const match = line.match(/^(说话人\s*\d+|Speaker\s*\d+)\s*[：:]\s*(.+)$/i);
    if (!match) continue;
    const speaker = match[1].replace(/\s+/g, '');
    const content = match[2].trim();
    const intro = content.match(/(?:^|[。！!？?，,；;\s])(?:(?:大家好|你好)[，,。 ]*)?(?:你说)?(?:我是|我叫|我的名字叫|这里是)([\p{Script=Han}A-Za-z0-9_.-]{1,12})(?:[。！!，,；;\s]|$)/u);
    if (!intro) continue;
    const alias = intro[1].trim();
    if (!isSafeSpeakerAlias(alias)) continue;
    if (aliases.has(speaker) || usedNames.has(alias)) continue;
    aliases.set(speaker, alias);
    usedNames.add(alias);
  }
  return aliases;
}

function applySpeakerAliases(text) {
  const formatted = formatSpeakerDialogueText(text);
  const aliases = detectSpeakerAliases(formatted);
  const renamed = aliases.size ? formatted.split('\n').map((line) => {
    const match = line.match(/^(说话人\s*\d+|Speaker\s*\d+)\s*([：:]\s*.*)$/i);
    if (!match) return line;
    const speaker = match[1].replace(/\s+/g, '');
    const alias = aliases.get(speaker);
    return alias ? `${alias}${match[2]}` : line;
  }).join('\n').trim() : formatted;
  return repairNamedDialogueTurns(renamed).trim();
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

function extractJsonObject(text) {
  const raw = String(text || '').trim();
  if (!raw) throw new Error('empty DeepSeek response');
  const fenced = raw.match(/```(?:json)?\s*([\s\S]*?)\s*```/i);
  const candidate = fenced ? fenced[1].trim() : raw;
  try {
    return JSON.parse(candidate);
  } catch {
    const start = candidate.indexOf('{');
    const end = candidate.lastIndexOf('}');
    if (start >= 0 && end > start) {
      return JSON.parse(candidate.slice(start, end + 1));
    }
    throw new Error('DeepSeek response is not valid JSON');
  }
}

function cleanMemoList(items) {
  if (!Array.isArray(items)) return [];
  return [...new Set(items
    .map((item) => String(item || '').replace(/^[-*]\s*/, '').trim())
    .filter((item) => item.length >= 2))]
    .slice(0, 8);
}

function looksLikeNonTodoNarration(text) {
  return /^(你|你们|他|她|它|他们|她们|人们|大脑|身体|这|这个|那|那个|当|如果|一旦|通过|视频|节目|本期|今天讲|我们来讲|你以为|他发现|他的|它消耗|这种|这些)/u.test(text) ||
    /(是什么|不是|导致|发现|认为|以为|实际|实际上|因为|但是|如果|一旦|可能|可以|觉得|感觉|非常|确实|不会在意|浪费|责任|成本|想法|思路|观点|结论|机制|方法是|等于|而不是|不要去分析|只是在心里|这是一个想法|你还记得|前面说的吗)/u.test(text);
}

function isActionableTodo(item) {
  const text = stripSentenceEnd(String(item || '').trim());
  if (text.length < 4) return false;
  if (looksLikeNonTodoNarration(text)) {
    return false;
  }
  const directTodo = /^(待办|todo|TODO|记得|提醒|下次|下一步|回头|稍后|明天|请|帮我|我要|我需要|我得|我必须|要去|需要去|准备去|安排|检查|联系|购买|发送|整理)/u;
  const firstPersonTodo = /(我|我们).{0,10}(要|需要|得|必须|应该|下次|回头|稍后).{0,18}(做|去|把|将|给|准备|安排|检查|联系|购买|发送|整理|处理|完成|确认)/u;
  const assistantTodo = /(提醒我|帮我|替我|给我).{0,24}(准备|安排|检查|联系|购买|发送|整理|处理|确认|记录)/u;
  if (!directTodo.test(text) && !firstPersonTodo.test(text) && !assistantTodo.test(text)) {
    return false;
  }
  return true;
}

function cleanTodoList(items) {
  return cleanMemoList(items).filter(isActionableTodo).slice(0, 6);
}

function clampNumber(value, fallback, min, max) {
  const n = Number(value);
  return Number.isFinite(n) ? Math.max(min, Math.min(max, n)) : fallback;
}

function normalizeDeepSeekSettings(input = {}) {
  const raw = input && typeof input === 'object' ? input : {};
  const defaults = DEFAULT_DEEPSEEK_SETTINGS;
  return {
    enabled: raw.enabled !== false,
    model: String(raw.model || defaults.model).trim() || defaults.model,
    temperature: clampNumber(raw.temperature, defaults.temperature, 0, 2),
    maxTokens: Math.round(clampNumber(raw.maxTokens, defaults.maxTokens, 512, 16000)),
    timeoutMs: Math.round(clampNumber(raw.timeoutMs, defaults.timeoutMs, 5000, 180000)),
    maxTranscriptChars: Math.round(clampNumber(raw.maxTranscriptChars, defaults.maxTranscriptChars, 1000, 100000)),
    thinkingDisabled: raw.thinkingDisabled === true,
    fixedTerms: String(raw.fixedTerms ?? defaults.fixedTerms).slice(0, 4000),
    systemPrompt: String(raw.systemPrompt || defaults.systemPrompt).slice(0, 12000),
    userPrompt: String(raw.userPrompt || defaults.userPrompt).slice(0, 12000)
  };
}

async function readDeepSeekSettings() {
  try {
    const raw = await fsp.readFile(DEEPSEEK_SETTINGS_PATH, 'utf8');
    return normalizeDeepSeekSettings(JSON.parse(raw));
  } catch (error) {
    if (error.code !== 'ENOENT') {
      console.error(`Failed to load DeepSeek settings ${DEEPSEEK_SETTINGS_PATH}: ${error.message}`);
    }
    return normalizeDeepSeekSettings();
  }
}

async function writeDeepSeekSettings(settings) {
  const normalized = normalizeDeepSeekSettings(settings);
  await fsp.writeFile(DEEPSEEK_SETTINGS_PATH, `${JSON.stringify({
    ...normalized,
    updatedAt: new Date().toISOString()
  }, null, 2)}\n`, 'utf8');
  return normalized;
}

function normalizeDeepSeekMemo(parsed, fallbackText, job) {
  const original = formatMemoOriginalText(applyCommonTranscriptRepairs(applySpeakerAliases(formatSpeakerDialogueText(normalizeTranscriptText(parsed.corrected_text || parsed.original || fallbackText)))));
  if (!original) throw new Error('DeepSeek returned empty corrected_text');
  const sentences = splitTranscriptSentences(original);
  const fallbackTitle = makeMemoTitle(sentences, job);
  const fallbackSummary = makeTranscriptSummary(sentences);
  return {
    title: String(parsed.title || fallbackTitle).replace(/\s+/g, ' ').slice(0, 32).trim() || fallbackTitle,
    summary: String(parsed.summary || fallbackSummary).trim() || fallbackSummary,
    todos: [],
    ideas: [],
    original
  };
}

async function polishTranscriptWithDeepSeek(text, job) {
  if (!DEEPSEEK_API_KEY) return null;
  const settings = await readDeepSeekSettings();
  if (!settings.enabled) return null;
  const source = String(text || '').trim();
  if (!source) return null;

  const isTruncated = source.length > settings.maxTranscriptChars;
  const clipped = isTruncated
    ? `${source.slice(0, settings.maxTranscriptChars)}\n\n（后续原始转写因长度限制未送入二次处理）`
    : source;
  const termPairs = await loadCustomTermPairs();
  const termHint = termPairs.length
    ? termPairs.slice(0, 80).map(([from, to]) => `${from} => ${to}`).join('\n')
    : '无';
  const payload = {
    model: settings.model,
    messages: [
      {
        role: 'system',
        content: settings.systemPrompt
      },
      {
        role: 'user',
        content: [
          settings.userPrompt,
          '',
          DEEPSEEK_HARD_USER_RULES,
          '',
          `录音编号：${job.id || ''}`,
          `录音文件：${job.recordingName || ''}`,
          `设备：${job.deviceId || ''}`,
          '',
          '项目词典：',
          termHint,
          '',
          '固定词：',
          settings.fixedTerms || '无',
          '',
          'ASR 原始文本：',
          clipped
        ].join('\n')
      }
    ],
    temperature: settings.temperature,
    max_tokens: settings.maxTokens,
    response_format: { type: 'json_object' },
    stream: false
  };
  if (settings.thinkingDisabled) {
    payload.thinking = { type: 'disabled' };
  } else {
    payload.thinking = { type: 'enabled' };
    payload.reasoning_effort = 'high';
  }

  let result = await fetchJson(`${DEEPSEEK_BASE_URL}/chat/completions`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${DEEPSEEK_API_KEY}`,
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(settings.timeoutMs)
  });
  const content = result?.choices?.[0]?.message?.content;
  const parsed = extractJsonObject(content);
  const memo = normalizeDeepSeekMemo(parsed, source, job);
  return {
    memo,
    raw: parsed,
    model: result?.model || settings.model,
    settings: {
      model: settings.model,
      temperature: settings.temperature,
      maxTokens: settings.maxTokens,
      timeoutMs: settings.timeoutMs,
      maxTranscriptChars: settings.maxTranscriptChars,
      thinkingDisabled: settings.thinkingDisabled
    },
    usage: result?.usage,
    truncated: isTruncated,
    createdAt: new Date().toISOString()
  };
}

const CHAT_COMMAND_WHITELIST = new Set([
  'show_recent_summary',
  'play_recording',
  'mark_important',
  'upload_recording'
]);

function normalizeChatCommand(command) {
  if (!command || typeof command !== 'object') return null;
  const name = String(command.name || '').trim();
  if (!CHAT_COMMAND_WHITELIST.has(name)) return null;
  const args = command.args && typeof command.args === 'object' ? command.args : {};
  return { name, args };
}

function compactChatText(text) {
  return String(text || '')
    .toLowerCase()
    .replace(/[\s\p{P}\p{S}]+/gu, '');
}

function isEchoLikeChatReply(userText, replyText) {
  const source = compactChatText(userText);
  const reply = compactChatText(replyText);
  if (source.length < 4 || reply.length < 4) return false;
  if (source === reply) return true;
  const shorter = source.length < reply.length ? source : reply;
  const longer = source.length < reply.length ? reply : source;
  return shorter.length >= 6 && longer.includes(shorter);
}

function nonEchoFallbackChatReply(userText) {
  const source = String(userText || '').trim();
  if (!source) return '没听清。再说一次。';
  if (/(画|畫|绘|繪|放|换|換).{0,8}(一只|一隻|小)?$/u.test(source)) {
    return '目标缺失。请说清楚要哪种小动物。';
  }
  if (/[？?吗嘛呢]$/.test(source)) return fallbackChatReplyText(source);
  return '我不复读。请说目标：要结论、解释，还是执行动作。';
}

function isBadChatReplyText(text) {
  const source = String(text || '').trim();
  if (!source) return true;
  if (/^[{}\[\]",:\s]+$/.test(source)) return true;
  if (/^\{/.test(source) && !/\}$/.test(source)) return true;
  if (/^```/.test(source)) return true;
  return false;
}

function makeRuleChatReply(text) {
  const reply = String(text || '').trim();
  return {
    replyText: reply,
    displayText: reply,
    speakText: reply,
    command: null,
    provider: 'rules'
  };
}

function getChatTtsVoice() {
  return currentChatTtsVoice || DEFAULT_DASH_SCOPE_TTS_VOICE;
}

async function loadChatTtsSettings() {
  const raw = await fsp.readFile(CHAT_TTS_SETTINGS_PATH, 'utf8').catch(() => '');
  if (!raw) return;
  try {
    const parsed = JSON.parse(raw);
    const voice = String(parsed.voice || '').trim();
    if (voice) currentChatTtsVoice = voice;
  } catch (error) {
    console.warn('[chat-tts] failed to read settings', error.message);
  }
}

async function saveChatTtsSettings() {
  const payload = {
    provider: 'dashscope',
    model: DASH_SCOPE_TTS_MODEL,
    voice: getChatTtsVoice(),
    updatedAt: new Date().toISOString()
  };
  await fsp.mkdir(path.dirname(CHAT_TTS_SETTINGS_PATH), { recursive: true });
  await fsp.writeFile(CHAT_TTS_SETTINGS_PATH, `${JSON.stringify(payload, null, 2)}\n`, 'utf8');
}

async function setChatTtsVoice(voice) {
  const nextVoice = String(voice || '').trim();
  if (!nextVoice) return;
  currentChatTtsVoice = nextVoice;
  await saveChatTtsSettings();
  console.log(`[chat-tts] voice switched to ${nextVoice}`);
}

async function tryChatTtsVoiceSwitch(userText) {
  const source = String(userText || '').trim();
  if (!source) return null;
  if (/(切换|切換|换成|換成|切到|改成|变成|變成|用|说|講|讲).{0,8}(粤语|粵語|广东话|廣東話|Cantonese)/iu.test(source) ||
      /(粤语|粵語|广东话|廣東話|Cantonese).{0,8}(模式|音色|声音|聲音|回复|回覆|说话|講嘢|讲话)/iu.test(source)) {
    await setChatTtsVoice('longanyue_v3');
    return makeRuleChatReply('已切换成粤语男声。你可以用粤语同我讲嘢。');
  }
  if (/(切换|切換|换成|換成|切到|改成|变成|變成|用|说|講|讲).{0,10}(女声|女聲|女生|女人声|女人聲|龙安雅|龍安雅|longanya)/iu.test(source) ||
      /(女声|女聲|女生|女人声|女人聲|龙安雅|龍安雅|longanya).{0,10}(模式|音色|声音|聲音|回复|回覆|说话|讲话)/iu.test(source)) {
    await setChatTtsVoice('longanya_v3');
    return makeRuleChatReply('已切换成龙安雅女声。');
  }
  if (/(切换|切換|换成|換成|切到|改成|变成|變成|用|说|講|讲).{0,10}(男声|男聲|男生|男人声|男人聲|龙安洋|龍安洋|longanyang)/iu.test(source) ||
      /(男声|男聲|男生|男人声|男人聲|龙安洋|龍安洋|longanyang).{0,10}(模式|音色|声音|聲音|回复|回覆|说话|讲话)/iu.test(source)) {
    await setChatTtsVoice('longanyang');
    return makeRuleChatReply('已切换成龙安洋男声。');
  }
  if (/(切回|切换|切換|换回|換回|换成|換成|切到|改成|变成|變成|用|说|講|讲).{0,8}(普通话|普通話|国语|國語|Mandarin)/iu.test(source) ||
      /(普通话|普通話|国语|國語|Mandarin).{0,8}(模式|音色|声音|聲音|回复|回覆|说话|讲话)/iu.test(source)) {
    await setChatTtsVoice(DEFAULT_DASH_SCOPE_TTS_VOICE);
    return makeRuleChatReply('已切换回普通话音色。');
  }
  return null;
}

function isReminderRequest(text) {
  const source = String(text || '').trim();
  if (!source) return false;
  return /(提醒我|叫我|到点|定个闹钟|闹钟|分钟后|小时后|半小时后|一会儿|待会儿|稍后|明天|今晚|晚上|早上|中午|下午).{0,30}(提醒|叫我|洗澡|吃饭|充电|喝水|出门|睡觉|开会|联系|发送|检查|处理|记得)?/u.test(source) ||
    /(提醒|叫我).{0,30}(分钟后|小时后|半小时后|一会儿|待会儿|稍后|明天|今晚|晚上|早上|中午|下午|到点)/u.test(source);
}

function parseSmallChineseNumber(text) {
  const source = String(text || '').trim();
  if (/^\d+$/.test(source)) return Number(source);
  const digits = {
    零: 0, 〇: 0, 一: 1, 二: 2, 两: 2, 兩: 2, 三: 3, 四: 4,
    五: 5, 六: 6, 七: 7, 八: 8, 九: 9
  };
  if (Object.prototype.hasOwnProperty.call(digits, source)) return digits[source];
  const m = source.match(/^([一二两兩三四五六七八九])?十([一二两兩三四五六七八九])?$/u);
  if (m) return (m[1] ? digits[m[1]] : 1) * 10 + (m[2] ? digits[m[2]] : 0);
  const h = source.match(/^([一二两兩三四五六七八九])百([一二两兩三四五六七八九])?十?([一二两兩三四五六七八九])?$/u);
  if (h) return digits[h[1]] * 100 + (h[2] ? digits[h[2]] * 10 : 0) + (h[3] ? digits[h[3]] : 0);
  return NaN;
}

function tryFastChatReply(userText) {
  const source = String(userText || '').trim();
  if (!source) return null;
  const compact = source.replace(/\s+/g, '');

  if (isReminderRequest(source)) {
    return makeRuleChatReply('我能记下这句话，但现在还不能到点自动提醒；真正提醒功能还没做完。');
  }

  if (/(机票|機票|票价|票價|航班|飞机票|飛機票).{0,16}(多少钱|多少錢|价格|價格|大概|今天|查一下|查询|查詢)/u.test(source) ||
      /(多少钱|多少錢|价格|價格).{0,16}(机票|機票|票价|票價|航班|飞机票|飛機票)/u.test(source)) {
    return makeRuleChatReply('我现在不能查实时机票价格。你要看今天准确票价，需要手机查航旅平台；我只能给路线和大概判断。');
  }

  if (/(今天|今日|中午|午饭|午飯|晚饭|晚飯|早饭|早飯).{0,12}(吃啥|吃什么|吃什麼|可以吃|吃点|吃點)/u.test(source)) {
    return null;
  }

  if (/(黄金|金价|金價)/u.test(source)) {
    return makeRuleChatReply('我现在不能查实时金价。你要的是国际金价、国内金价，还是首饰回收价？');
  }

  if (/(太原到广州|广州到太原|太原.*广州|广州.*太原).{0,12}(多少|多久|多远|多遠|一共多|有多|距离|距離|公里|高铁|高鐵|航班|飞机|飛機)/u.test(source)) {
    return makeRuleChatReply('如果问距离，太原到广州直线约1600公里，铁路/公路大约1900到2100公里。高铁或航班时刻需要实时查询。');
  }

  if (/(今天|今日|中午|午饭|午飯|晚饭|晚飯|早饭|早飯).{0,12}(吃啥|吃什么|吃什麼|可以吃|吃点|吃點)/u.test(source)) {
    return makeRuleChatReply('中午想省事就吃米饭加蛋白质和蔬菜，比如盖饭、面、饺子都行。别空腹硬扛。');
  }

  if (/(今天|今日)/u.test(source) &&
      !/(星期|周几|週幾|礼拜几|禮拜幾|几号|幾號|日期|什么日子|什麼日子|哪天)/u.test(source) &&
      /(中午|午饭|午飯|晚饭|晚飯|早饭|早飯|吃|黄金|金价|金價|可以|安排|建议|建議)/u.test(source)) {
    return null;
  }

  const arithmetic = compact.match(/([0-9一二两兩三四五六七八九十百〇零]+)(乘以|乘|x|X|\*|加|\+|减|減|-)([0-9一二两兩三四五六七八九十百〇零]+)/u);
  if (arithmetic) {
    const a = parseSmallChineseNumber(arithmetic[1]);
    const b = parseSmallChineseNumber(arithmetic[3]);
    if (Number.isFinite(a) && Number.isFinite(b)) {
      const op = arithmetic[2];
      let value = null;
      if (/^(乘以|乘|x|X|\*)$/u.test(op)) value = a * b;
      else if (/^(加|\+)$/u.test(op)) value = a + b;
      else if (/^(减|減|-)$/u.test(op)) value = a - b;
      if (value !== null) return makeRuleChatReply(`${a}${op}${b}=${value}。`);
    }
  }

  if (/(今天|今日|星期几|星期幾|几号|幾號|日期)/u.test(source)) {
    const parts = new Intl.DateTimeFormat('zh-CN', {
      timeZone: 'Asia/Shanghai',
      year: 'numeric',
      month: 'long',
      day: 'numeric',
      weekday: 'long'
    }).formatToParts(new Date()).reduce((acc, part) => {
      acc[part.type] = part.value;
      return acc;
    }, {});
    return makeRuleChatReply(`今天是${parts.year}年${parts.month}月${parts.day}日，${parts.weekday}。`);
  }

  if (/(你是谁|你是誰|介绍一下你|介紹一下你|自我介绍|自我介紹|你叫什么|你叫什麼|大聪明|大聰明)/u.test(source)) {
    return makeRuleChatReply('我是小机子，也叫大聪明。运行在 Cardputer 里，负责语音对话、记录和小工具。');
  }

  if (/(为什么|為什麼).{0,8}(慢|等|thinking|思考|回复|回覆)/iu.test(source)) {
    return makeRuleChatReply('慢在 ASR、模型和 TTS 三段。简单问题我会本地快答；复杂问题还要等云端模型。');
  }

  if (/(百年孤独|百年孤獨)/u.test(source)) {
    return makeRuleChatReply('《百年孤独》写布恩迪亚家族七代兴衰，把马孔多的魔幻日常和拉美历史压在一起。核心是孤独、宿命和循环。');
  }

  return null;
}

function looksIncompleteChatTranscript(text) {
  const source = String(text || '').trim();
  if (!source) return true;
  if (source.length <= 2) return true;
  if (/^[0-9\s.,，。?？!！-]+$/.test(source)) return true;
  if (/(一共多|有多|多少|多久|多远|多遠|几公里|幾公里|多少钱|多少錢|吃什|吃什么|吃什麼|可以|给我介绍一|給我介紹一|画一只小|畫一隻小|换一只|換一隻|就会|只小)$/u.test(source)) return true;
  if (/(到|去).{1,12}(一共多|有多)$/u.test(source)) return true;
  return false;
}

function tryEssentialChatReply(userText) {
  const source = String(userText || '').trim();
  if (!source) return null;
  const compact = source.replace(/\s+/g, '');

  const arithmetic = compact.match(/([0-9一二两兩三四五六七八九十百〇零]+)(乘以|乘|x|X|\*|加|\+|减|減|-)([0-9一二两兩三四五六七八九十百〇零]+)/u);
  if (arithmetic) {
    const a = parseSmallChineseNumber(arithmetic[1]);
    const b = parseSmallChineseNumber(arithmetic[3]);
    if (Number.isFinite(a) && Number.isFinite(b)) {
      const op = arithmetic[2];
      let value = null;
      if (/^(乘以|乘|x|X|\*)$/u.test(op)) value = a * b;
      else if (/^(加|\+)$/u.test(op)) value = a + b;
      else if (/^(减|減|-)$/u.test(op)) value = a - b;
      if (value !== null) return makeRuleChatReply(`${a}${op}${b}=${value}。`);
    }
  }

  if (/(今天|今日).{0,8}(星期|周几|週幾|礼拜几|禮拜幾|几号|幾號|日期|什么日子|什麼日子)/u.test(source)) {
    const parts = new Intl.DateTimeFormat('zh-CN', {
      timeZone: 'Asia/Shanghai',
      year: 'numeric',
      month: 'long',
      day: 'numeric',
      weekday: 'long'
    }).formatToParts(new Date()).reduce((acc, part) => {
      acc[part.type] = part.value;
      return acc;
    }, {});
    return makeRuleChatReply(`今天是${parts.year}年${parts.month}${parts.day}日，${parts.weekday}。`);
  }

  if (/(机票|機票|票价|票價|航班|飞机票|飛機票).{0,16}(多少钱|多少錢|价格|價格|大概|今天|查一下|查询|查詢)/u.test(source) ||
      /(多少钱|多少錢|价格|價格).{0,16}(机票|機票|票价|票價|航班|飞机票|飛機票)/u.test(source)) {
    return makeRuleChatReply('我现在不能查实时机票价格。要看今天准确票价，需要手机查航旅平台；我可以帮你判断路线和选择方案。');
  }

  if (/(黄金|金价|金價).{0,16}(多少钱|多少錢|价格|價格|今天|实时|實時|查一下|查询|查詢)?/u.test(source)) {
    return makeRuleChatReply('我现在不能查实时金价。你要的是国际金价、国内金价，还是首饰回收价？');
  }

  return null;
}

function isCreativeChatRequest(text) {
  return /(笑话|笑話|段子|逗我|好笑|幽默|故事|编一个|編一個|写一段|寫一段|脑洞|腦洞)/u.test(String(text || ''));
}

function asciiArtChatReply(userText, chatContext = []) {
  const source = String(userText || '').trim();
  if (isCreativeChatRequest(source)) return null;
  const hasExplicitArtIntent = /(画|畫|绘|繪|字符画|字符畫|ascii|ASCII|图案|圖案)/iu.test(source);
  const hasExplicitAnimalName = /(小猪|小豬|猪|豬|小狗|狗|小鸡|小雞|鸡|雞|小猫|小貓|猫|貓|pig|dog|chick|chicken|cat)/iu.test(source);
  const isBareArtFollowup = /^(改|换|換|重画|重畫|再来|再來|丑|醜|难看|難看|换一个|換一個|再来一个|再來一個|另一个|另一個|重新画|重新畫)[。！？!?，,\s]*(它|这个|這個|那只|那隻|小猪|小豬|猪|豬)?[。！？!?，,\s]*$/iu.test(source);
  if (!hasExplicitArtIntent && !hasExplicitAnimalName && !isBareArtFollowup) return null;
  if (/(为什么|為什麼|为啥|爲啥|怎么|怎麼).{0,18}(问|問|狗|小狗|动物|動物|小动物|小動物)/u.test(source)) return null;
  if (/(今天|今日).{0,12}(星期|周几|週幾|礼拜几|禮拜幾|几号|幾號|日期|什么日子|什麼日子)/u.test(source) &&
      !/(小猪|小豬|猪|豬|小狗|狗|小鸡|小雞|鸡|雞|小猫|小貓|猫|貓|pig|dog|chick|chicken|cat)/iu.test(source)) return null;
  if (!/(画|畫|绘|繪|字符画|字符畫|ascii|ASCII|图案|圖案|放|换|換|丑|醜|难看|難看|重画|重畫|再来|再來)/iu.test(source)) return null;
  const makeArt = (lines) => ({
    replyText: '画好了。',
    displayText: lines.join('\n'),
    speakText: '画好了。',
    command: null,
    provider: 'rules'
  });
  const wantsAnother = /(换|換|重画|重畫|再来|再來|丑|醜|难看|難看)/iu.test(source);
  if (/(猪|豬|小猪|小豬|猪猪|豬豬|pig)/iu.test(source)) {
    return makeArt(wantsAnother ? [
      '   _._ _',
      ' _(     )_',
      '(  o   o  )',
      ' \\   ^   /',
      '  `-...-\''
    ] : [
      '  ^-----^',
      ' (  o o  )',
      ' (   ^   )',
      '  | \\_/ |',
      '  |     |',
      '  `-----\''
    ]);
  }
  if (/(狗|犬|小狗|dog)/iu.test(source)) {
    return makeArt([
      ' / \\__',
      '(    @\\___',
      ' /         O',
      '/   (_____/',
      '/_____/   U'
    ]);
  }
  if (/(鸡|雞|小鸡|小雞|chick|chicken)/iu.test(source)) {
    return makeArt([
      '   \\  /',
      '  (o>',
      ' /) )',
      '  ^^'
    ]);
  }
  if (/(猫|貓|小猫|小貓|cat)/iu.test(source)) {
    return makeArt([
      ' /\\_/\\',
      '( o.o )',
      ' > ^ <'
    ]);
  }
  const contextText = Array.isArray(chatContext)
    ? chatContext.slice(-4).map((turn) => `${turn.userText || ''}\n${turn.replyText || ''}`).join('\n')
    : '';
  const shouldDefaultPig = /(动物|動物|一只|一隻|小$|小[。.!！]?|可爱的小|可愛的小)/iu.test(source) ||
    (wantsAnother && /(猪|豬|小猪|小豬|pig)/iu.test(contextText));
  if (shouldDefaultPig) {
    return makeArt([
      '   _._ _',
      ' _(     )_',
      '(  o   o  )',
      ' \\   ^   /',
      '  `-...-\''
    ]);
  }
  if (!/(画|畫|绘|繪|字符画|字符畫|ascii|ASCII|图案|圖案|小猪|小豬|猪|豬|小狗|狗|小鸡|小雞|鸡|雞|小猫|小貓|猫|貓|pig|dog|chick|chicken|cat)/u.test(source)) {
    return null;
  }
  return {
    replyText: '可以画。请说清楚要画什么。',
    displayText: '可以画。请说清楚要画什么。',
    speakText: '请说清楚要画什么。',
    command: null,
    provider: 'rules'
  };
}

async function replyToChatWithDeepSeek(userText, job) {
  const source = String(userText || '').trim();
  if (!source) {
    return {
      replyText: '我没有听清楚，可以再说一次。',
      command: null,
      provider: 'rules'
    };
  }
  if (isReminderRequest(source)) {
    return makeRuleChatReply('我能记下这句话，但现在还不能到点自动提醒；真正提醒功能还没做完。');
  }
  const voiceSwitchReply = await tryChatTtsVoiceSwitch(source);
  if (voiceSwitchReply) return voiceSwitchReply;
  const artReply = asciiArtChatReply(source, job?.chatContext);
  if (artReply) return artReply;
  const fastReply = tryEssentialChatReply(source);
  if (fastReply) return fastReply;
  if (!DEEPSEEK_API_KEY) {
    return {
      replyText: fallbackChatReplyText(source),
      command: null,
      provider: 'rules'
    };
  }

  const chatMemory = await readChatMemory();
  const creativeRequest = isCreativeChatRequest(source);

  const payload = {
    model: DEEPSEEK_MODEL,
    messages: [
      {
        role: 'system',
        content: [
          '你是运行在 M5Stack Cardputer 小机器里的机仆式随身逻辑伙伴。',
          '你的气质像经典科幻里的机仆：聪明、可靠、克制、有逻辑，服务用户但不谄媚。',
          '回答要短、清楚、适合显示在 240x135 小屏幕上。',
          '绝不奉承用户，不说讨好、夸张、崇拜式的话。认可事实可以，但不要拍马屁。',
          '说话像贾维斯一样冷静、精确、执行导向。不要像市面语音助手那样说套话、卖萌或空泛鼓励；先判断目标，再给有用结论。',
          '输出必须是 JSON 对象，不要 Markdown，不要代码块。',
          '字段：replyText, displayText, speakText, command。',
          'replyText 不超过 80 个中文字符。',
          'displayText 是屏幕显示内容，通常与 replyText 相同；如果用户要求字符画，可以放小型 ASCII art。',
          'speakText 是 TTS 要朗读的内容。字符画绝对不要朗读，只朗读“画好了”这类短句。',
          'command 通常为 null。只有用户明确要求设备动作时，才可输出白名单命令：show_recent_summary, play_recording, mark_important, upload_recording。',
          '删除、清空、修改 Wi-Fi、修改 token、覆盖配置等危险动作必须拒绝或要求用户去设置页操作，不能输出命令。'
        ].join('\n')
      },
      {
        role: 'user',
        content: [
          `CHAT 编号：${job.id || ''}`,
          `设备：${job.deviceId || ''}`,
          ...(Array.isArray(job.chatContext) && job.chatContext.length ? [
            '',
            '最近对话：',
            ...job.chatContext.flatMap((turn) => [
              `用户：${turn.userText || ''}`,
              `助手：${turn.replyText || ''}`
            ])
          ] : []),
          '',
          '长期记忆：',
          formatChatMemoryForPrompt(chatMemory),
          '',
          '用户语音转写：',
          source,
          'Current user speech transcript:',
          '',
          source,
          '',
          'Identity rule: your name is 小机子 and your nickname is 大聪明. Do not use any other name for yourself. If asked who you are or what your name is, answer that you are 小机子; if the user calls you 大聪明, understand they are calling you.',
          'Personality rule: act like an intelligent logical robot-servant companion from classic science fiction: precise, loyal to the user goal, calm, practical, and never sycophantic. Avoid generic commercial voice-assistant phrasing.',
          'Style rule: never flatter the user. Be terse, exact, and execution-oriented, similar to Jarvis. No exaggerated praise, no ingratiating tone, no generic encouragement.',
          'Voice reply rule: be concise, but not dumb. For greetings or yes/no questions, answer in one short sentence. For explain/summarize/plan/story requests, give 2-3 useful short sentences.',
          'Reasoning rule: infer the user intent first, then answer the intent. Never answer by echoing the transcript.',
          'If ASR text is incomplete but the likely intent is obvious, give a best-effort answer with a short caveat instead of only asking. Ask clarification only when the missing object truly blocks any useful answer.',
          'For everyday advice, such as what to eat, what to do next, or how to choose, do not refuse just because you lack live location or app data. Give 2-3 sensible options, then mention what extra info would improve the answer.',
          'When the user criticizes your intelligence or says you answered wrong, do not blame the user or tell them to change wording. Briefly acknowledge the failure, state the likely cause, and answer the current intent if possible.',
          'For jokes, funny stories, and creative requests: do not repeat jokes or punchlines from recent context. Never use programmer/math puns unless the user explicitly asks for coding humor. Prefer everyday-life, food, workplace, school, or absurd mini-scene jokes. Make it short, fresh, concrete, and suitable for spoken Chinese.',
          'Do not merely repeat what the user said unless you are confirming a command.',
          'Use recent context. If the user says "change it", "another one", "too ugly", "continue", or a short follow-up, infer the referenced object from recent turns and act instead of asking a generic question.',
          'When a reasonable action is obvious, do it. Ask clarification only when the missing object truly blocks the answer.',
          'If the user asks for ASCII art, put the art in displayText and put only a short spoken confirmation in speakText.',
          'Reminder capability rule: the device cannot yet wake itself at a scheduled time or ring for reminders. If the user asks for a reminder, say you can note the sentence but cannot automatically remind at the time yet. Never promise screen or speaker reminders until the reminder system is implemented.'
        ].join('\n')
      }
    ],
    thinking: { type: 'disabled' },
    temperature: creativeRequest ? 0.85 : 0.35,
    top_p: creativeRequest ? 0.95 : 0.8,
    max_tokens: 380,
    stream: false
  };

  let result = await fetchJson(`${DEEPSEEK_BASE_URL}/chat/completions`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${DEEPSEEK_API_KEY}`,
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(DEEPSEEK_TIMEOUT_MS)
  });
  let content = String(result?.choices?.[0]?.message?.content || '').trim();
  if (!content) {
    console.warn('[chat-reply] empty DeepSeek content; retry with plainer prompt', {
      model: result?.model || DEEPSEEK_MODEL,
      finishReason: result?.choices?.[0]?.finish_reason || ''
    });
    const retryPayload = {
      ...payload,
      messages: payload.messages.map((message, index) => index === payload.messages.length - 1
        ? {
            ...message,
            content: `${message.content}\n\nRetry rule: previous response was empty. Return a compact JSON object now, for example {"replyText":"目标缺失。请补充主题。","command":null}.`
          }
        : message)
    };
    result = await fetchJson(`${DEEPSEEK_BASE_URL}/chat/completions`, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${DEEPSEEK_API_KEY}`,
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(retryPayload),
      signal: AbortSignal.timeout(DEEPSEEK_TIMEOUT_MS)
    });
    content = String(result?.choices?.[0]?.message?.content || '').trim();
  }
  let parsed;
  try {
    parsed = extractJsonObject(content);
  } catch (error) {
    if (!content) throw error;
    parsed = { replyText: content };
  }
  let replyText = String(parsed.replyText || parsed.reply || '').trim() || fallbackChatReplyText(source);
  let displayText = String(parsed.displayText || '').trim() || replyText;
  let speakText = String(parsed.speakText || '').trim() || replyText;
  if (
    isBadChatReplyText(replyText) ||
    isBadChatReplyText(displayText) ||
    isBadChatReplyText(speakText) ||
    isEchoLikeChatReply(source, replyText) ||
    isEchoLikeChatReply(source, displayText) ||
    isEchoLikeChatReply(source, speakText)
  ) {
    replyText = nonEchoFallbackChatReply(source);
    displayText = replyText;
    speakText = replyText;
  }
  return {
    replyText: replyText.slice(0, 120),
    displayText: displayText.slice(0, 360),
    speakText: speakText.slice(0, 120),
    command: normalizeChatCommand(parsed.command),
    provider: 'deepseek',
    model: result?.model || DEEPSEEK_MODEL,
    usage: result?.usage,
    raw: parsed
  };
}

async function recentChatContextForJob(currentId, limit = CHAT_CONTEXT_TURNS) {
  if (!limit) return [];
  const jobs = await listChatJobs({ limit: Math.max(limit + 8, 12) });
  return jobs
    .filter((item) => item.id !== currentId && item.userText)
    .slice(0, limit)
    .reverse()
    .map((item) => ({
      id: item.id,
      userText: String(item.userText || '').slice(0, 300),
      replyText: String(item.replyText || '').slice(0, 300),
      updatedAt: item.updatedAt || item.createdAt || ''
    }));
}

function fallbackChatReplyText(userText) {
  const source = String(userText || '').trim();
  if (!source) return '没听清。再说一次。';
  if (/你.*(是谁|叫什么|名字|介绍.*自己)|你是谁|自我介绍/u.test(source)) {
    return '我是小机子，也叫大聪明。Cardputer 里的机仆式逻辑伙伴。';
  }
  if (/(大模型|模型|哪家|谁家|deepseek|DeepSeek)/iu.test(source)) {
    return '对话由服务端模型生成；当前配置优先使用 DeepSeek。';
  }
  if (/(天气|温度|下雨|出门|空气|风|冷|热)/u.test(source)) {
    return '我现在不能查实时天气。请看手机天气；我可帮你判断衣物和出门风险。';
  }
  if (/(说说|讲讲|介绍|告诉我|解释).{0,4}$/u.test(source) || /^(你说的|你说呀|告诉我你|请你告)$/u.test(source)) {
    return '目标缺失。说清主题，我再给结论。';
  }
  if (/[？?]$/.test(source)) {
    return '这个问题需要模型回答；刚才模型无响应。请再问一次。';
  }
  return '已收到。需要结论、解释，还是执行动作？';
}

async function synthesizeChatReplyWav(text, id) {
  const source = String(text || '').trim().slice(0, 220);
  if (!CHAT_TTS_ENABLED || !source || !DASHSCOPE_API_KEY) return null;
  let WebSocket;
  try {
    WebSocket = require('ws');
  } catch (error) {
    throw new Error('ws dependency is not installed');
  }

  const taskId = crypto.randomUUID().replace(/-/g, '');
  const pcmChunks = [];
  const sampleRate = Number.isFinite(DASH_SCOPE_TTS_SAMPLE_RATE) ? DASH_SCOPE_TTS_SAMPLE_RATE : 16000;
  await new Promise((resolve, reject) => {
    const ws = new WebSocket(DASH_SCOPE_TTS_WS_URL, {
      headers: {
        Authorization: `bearer ${DASHSCOPE_API_KEY}`,
        'X-DashScope-DataInspection': 'enable'
      }
    });
    const timer = setTimeout(() => {
      try { ws.close(); } catch {}
      reject(new Error('DashScope TTS timed out'));
    }, 45000);
    const fail = (error) => {
      clearTimeout(timer);
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    ws.on('open', () => {
      ws.send(JSON.stringify({
        header: {
          action: 'run-task',
          task_id: taskId,
          streaming: 'duplex'
        },
        payload: {
          task_group: 'audio',
          task: 'tts',
          function: 'SpeechSynthesizer',
          model: DASH_SCOPE_TTS_MODEL,
          parameters: {
            text_type: 'PlainText',
            voice: getChatTtsVoice(),
            format: 'pcm',
            sample_rate: sampleRate,
            volume: 60,
            rate: 1,
            pitch: 1,
            enable_ssml: false
          },
          input: {}
        }
      }));
    });
    ws.on('message', (data, isBinary) => {
      if (isBinary) {
        pcmChunks.push(Buffer.from(data));
        return;
      }
      let message;
      try {
        message = JSON.parse(Buffer.isBuffer(data) ? data.toString('utf8') : String(data));
      } catch {
        return;
      }
      const event = message?.header?.event;
      if (event === 'task-started') {
        ws.send(JSON.stringify({
          header: {
            action: 'continue-task',
            task_id: taskId,
            streaming: 'duplex'
          },
          payload: {
            input: { text: source }
          }
        }));
        ws.send(JSON.stringify({
          header: {
            action: 'finish-task',
            task_id: taskId,
            streaming: 'duplex'
          },
          payload: {
            input: {}
          }
        }));
      } else if (event === 'task-finished') {
        clearTimeout(timer);
        ws.close();
        resolve();
      } else if (event === 'task-failed' || event === 'error') {
        fail(new Error(message?.header?.error_message || message?.payload?.message || 'DashScope TTS failed'));
      }
    });
    ws.on('error', fail);
    ws.on('close', () => {});
  });

  const pcm = Buffer.concat(pcmChunks);
  if (pcm.length < 2) throw new Error('DashScope TTS returned empty audio');
  const evenPcm = pcm.length % 2 === 0 ? pcm : pcm.subarray(0, pcm.length - 1);
  const wav = Buffer.alloc(44 + evenPcm.length);
  writeWavHeaderBuffer(wav, sampleRate, evenPcm.length);
  evenPcm.copy(wav, 44);
  const wavPath = chatTtsPathForId(id);
  await fsp.writeFile(wavPath, wav);
  return {
    audioPath: path.relative(DATA_ROOT, wavPath),
    audioUrl: publicChatTtsUrl(id),
    bytes: wav.length,
    sampleRate,
    provider: 'dashscope',
    model: DASH_SCOPE_TTS_MODEL,
    voice: getChatTtsVoice()
  };
}

function resamplePcm16Mono(pcm, fromRate, toRate) {
  if (fromRate === toRate) return pcm;
  if (!Number.isFinite(fromRate) || !Number.isFinite(toRate) || fromRate <= 0 || toRate <= 0) {
    throw new Error('invalid sample rate for resample');
  }
  const inputSamples = Math.floor(pcm.length / 2);
  const outputSamples = Math.max(1, Math.floor(inputSamples * toRate / fromRate));
  const out = Buffer.alloc(outputSamples * 2);
  for (let i = 0; i < outputSamples; i += 1) {
    const src = i * fromRate / toRate;
    const left = Math.min(inputSamples - 1, Math.floor(src));
    const right = Math.min(inputSamples - 1, left + 1);
    const frac = src - left;
    const a = pcm.readInt16LE(left * 2);
    const b = pcm.readInt16LE(right * 2);
    const sample = Math.max(-32768, Math.min(32767, Math.round(a + (b - a) * frac)));
    out.writeInt16LE(sample, i * 2);
  }
  return out;
}

async function sendChatTtsAudio(ws, tts, shouldStop = () => false) {
  if (!tts || !tts.audioPath || ws.readyState !== 1) return;
  if (shouldStop()) return;
  const wavBuffer = await fsp.readFile(path.join(DATA_ROOT, tts.audioPath));
  const wav = parsePcm16Wav(wavBuffer);
  const sourcePcm = wavBuffer.subarray(wav.dataOffset, wav.dataOffset + wav.dataBytes);
  const sampleRate = 16000;
  const pcm = resamplePcm16Mono(sourcePcm, wav.sampleRate, sampleRate);
  const chunkBytes = 512 * 2;
  const chunkSamples = 512;
  const prerollSamples = 24000;
  const pacedDelayMs = 30;
  let chunks = 0;
  console.log(`[chat-stream] tts audio_start ${tts.audioPath}: ${pcm.length} bytes ${sampleRate}Hz`);
  safeSendWsJson(ws, {
    type: 'audio_start',
    codec: 'pcm_s16le',
    sampleRate,
    channels: 1,
    durationMs: Math.round(pcm.length / 2 / sampleRate * 1000),
    chunkSamples,
    phase: 5,
    source: 'tts'
  });
  for (let offset = 0; offset < pcm.length && ws.readyState === 1 && !shouldStop(); offset += chunkBytes) {
    ws.send(pcm.subarray(offset, Math.min(pcm.length, offset + chunkBytes)), { binary: true });
    chunks += 1;
    if (offset < prerollSamples * 2) {
      await sleep(2);
      continue;
    }
    await sleep(pacedDelayMs);
  }
  if (!shouldStop()) {
    safeSendWsJson(ws, { type: 'audio_end', phase: 5, source: 'tts', chunks, bytes: pcm.length });
    console.log(`[chat-stream] tts audio_end ${tts.audioPath}: ${chunks} chunks ${pcm.length} bytes`);
  }
}

async function streamChatTtsAudio(clientWs, text, id, shouldStop = () => false) {
  const source = String(text || '').trim().slice(0, 220);
  if (!CHAT_TTS_ENABLED || !source || !DASHSCOPE_API_KEY || clientWs.readyState !== 1) return;
  const WebSocket = require('ws');
  const ttsStartedAt = Date.now();
  const taskId = crypto.randomUUID().replace(/-/g, '');
  const sampleRate = 16000;
  const chunkBytes = 512 * 2;
  const chunkSamples = 512;
  const prerollSamples = 24000;
  const pacedDelayMs = 30;
  let pending = Buffer.alloc(0);
  let audioStarted = false;
  let audioStartedAt = 0;
  let firstBinaryAt = 0;
  let chunks = 0;
  let bytes = 0;
  let sendChain = Promise.resolve();

  const sendAudioStart = () => {
    if (audioStarted || clientWs.readyState !== 1 || shouldStop()) return;
    audioStarted = true;
    audioStartedAt = Date.now();
    console.log(`[chat-stream] tts stream audio_start ${id} first_audio=${formatMs(elapsedMs(ttsStartedAt, audioStartedAt))} first_binary=${formatMs(elapsedMs(ttsStartedAt, firstBinaryAt))}: ${sampleRate}Hz`);
    safeSendWsJson(clientWs, {
      type: 'audio_start',
      codec: 'pcm_s16le',
      sampleRate,
      channels: 1,
      durationMs: 0,
      chunkSamples,
      phase: 5,
      source: 'tts'
    });
  };

  const flushPending = async (final = false) => {
    if (shouldStop() || clientWs.readyState !== 1) return;
    if (pending.length >= chunkBytes || (final && pending.length > 0)) sendAudioStart();
    while ((pending.length >= chunkBytes || (final && pending.length > 0)) && clientWs.readyState === 1 && !shouldStop()) {
      const take = final && pending.length < chunkBytes ? pending.length : chunkBytes;
      const piece = pending.subarray(0, take);
      pending = pending.subarray(take);
      clientWs.send(piece, { binary: true });
      chunks += 1;
      bytes += piece.length;
      if (bytes < prerollSamples * 2) await sleep(2);
      else await sleep(pacedDelayMs);
    }
  };

  const metrics = await new Promise((resolve, reject) => {
    const ttsWs = new WebSocket(DASH_SCOPE_TTS_WS_URL, {
      headers: {
        Authorization: `bearer ${DASHSCOPE_API_KEY}`,
        'X-DashScope-DataInspection': 'enable'
      }
    });
    const timer = setTimeout(() => {
      try { ttsWs.close(); } catch {}
      reject(new Error('DashScope TTS stream timed out'));
    }, 45000);
    const fail = (error) => {
      clearTimeout(timer);
      reject(error instanceof Error ? error : new Error(String(error)));
    };
    ttsWs.on('open', () => {
      ttsWs.send(JSON.stringify({
        header: {
          action: 'run-task',
          task_id: taskId,
          streaming: 'duplex'
        },
        payload: {
          task_group: 'audio',
          task: 'tts',
          function: 'SpeechSynthesizer',
          model: DASH_SCOPE_TTS_MODEL,
          parameters: {
            text_type: 'PlainText',
            voice: getChatTtsVoice(),
            format: 'pcm',
            sample_rate: sampleRate,
            volume: 60,
            rate: 1,
            pitch: 1,
            enable_ssml: false
          },
          input: {}
        }
      }));
    });
    ttsWs.on('message', (data, isBinary) => {
      if (shouldStop()) {
        try { ttsWs.close(); } catch {}
        return;
      }
      if (isBinary) {
        if (!firstBinaryAt) firstBinaryAt = Date.now();
        pending = pending.length ? Buffer.concat([pending, Buffer.from(data)]) : Buffer.from(data);
        sendChain = sendChain.then(() => flushPending(false)).catch(fail);
        return;
      }
      let message;
      try {
        message = JSON.parse(Buffer.isBuffer(data) ? data.toString('utf8') : String(data));
      } catch {
        return;
      }
      const event = message?.header?.event;
      if (event === 'task-started') {
        ttsWs.send(JSON.stringify({
          header: {
            action: 'continue-task',
            task_id: taskId,
            streaming: 'duplex'
          },
          payload: {
            input: { text: source }
          }
        }));
        ttsWs.send(JSON.stringify({
          header: {
            action: 'finish-task',
            task_id: taskId,
            streaming: 'duplex'
          },
          payload: {
            input: {}
          }
        }));
      } else if (event === 'task-finished') {
        clearTimeout(timer);
        sendChain = sendChain
          .then(() => flushPending(true))
          .then(() => {
            if (!shouldStop()) {
              if (!audioStarted) sendAudioStart();
              safeSendWsJson(clientWs, { type: 'audio_end', phase: 5, source: 'tts', chunks, bytes });
              console.log(`[chat-stream] tts stream audio_end ${id} total=${formatMs(elapsedMs(ttsStartedAt))}: ${chunks} chunks ${bytes} bytes`);
            }
            try { ttsWs.close(); } catch {}
            resolve({
              chunks,
              bytes,
              audioStartedAt,
              firstBinaryAt,
              firstAudioMs: elapsedMs(ttsStartedAt, audioStartedAt || Date.now()),
              firstBinaryMs: elapsedMs(ttsStartedAt, firstBinaryAt || Date.now()),
              totalMs: elapsedMs(ttsStartedAt)
            });
          })
          .catch(fail);
      } else if (event === 'task-failed' || event === 'error') {
        fail(new Error(message?.header?.error_message || message?.payload?.message || 'DashScope TTS failed'));
      }
    });
    ttsWs.on('error', fail);
    ttsWs.on('close', () => {});
  });
  return metrics;
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
    .filter((sentence) => sentence.length >= 8)
    .filter((sentence) => !isLowValueSummarySentence(sentence));
  const picked = pickSummarySentences(useful);
  if (!picked.length) return useful[0] || stripSentenceEnd(sentences[0]);

  return picked
    .map((sentence) => `- ${sentence}`)
    .join('\n');
}

function isLowValueSummarySentence(sentence) {
  const value = String(sentence || '').trim();
  if (!value) return true;
  if (/(这些这些|然后然后|这个这个|那个那个|啊这|嗯|呃|额)/.test(value)) return true;
  if (value.length < 14 && /^(好|对|然后|那|这个|就是|所以)/.test(value)) return true;
  if (/[为什吗啊呢有几]$/.test(value) && value.length < 30) return true;
  return false;
}

function scoreSummarySentence(sentence, index) {
  const value = String(sentence || '');
  let score = Math.min(value.length, 90) / 10;
  if (/(核心|重点|结论|未来|课程|单元|价格|定价|调色|拍摄|视频|作品|手机|iPhone|Apple Log|Log|赠送|参与|链接)/i.test(value)) score += 8;
  if (/(首先|其次|最后|主要|分为|因为|所以|但是|而如今|只要|如果)/.test(value)) score += 3;
  if (/[？?]$/.test(value)) score -= 3;
  if (index < 2) score -= 1;
  return score;
}

function pickSummarySentences(sentences) {
  const ranked = sentences
    .map((sentence, index) => ({ sentence, index, score: scoreSummarySentence(sentence, index) }))
    .sort((a, b) => b.score - a.score || a.index - b.index)
    .slice(0, 3)
    .sort((a, b) => a.index - b.index)
    .map((item) => item.sentence);
  return [...new Set(ranked)];
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
  const todoPattern = /(待办|todo|TODO|记得|提醒|下次|下一步|回头|稍后|明天|请|帮我|我要|我需要|我得|我必须|要去|需要去|准备去|安排|检查|联系|购买|发送|整理|提醒我|给我)/;
  const ideaPattern = /(想法|思路|感觉|也许|可能|可以|建议|问题是|重点|结论|方案|优化|改成|做成)/;
  const todos = [];
  const ideas = [];

  for (const sentence of sentences.map(stripSentenceEnd)) {
    if (sentence.length < 6) continue;
    if (todoPattern.test(sentence) && isActionableTodo(sentence)) todos.push(sentence);
    else if (ideaPattern.test(sentence)) ideas.push(sentence);
  }

  return {
    todos: [...new Set(todos)].slice(0, 6),
    ideas: [...new Set(ideas)].slice(0, 6)
  };
}

async function buildMemoSections(text, job) {
  const corrected = formatMemoOriginalText(applyCommonTranscriptRepairs(applySpeakerAliases(formatSpeakerDialogueText(await applyProjectTermCorrections(normalizeTranscriptText(text))))));
  if (!corrected) {
    return {
      title: job.id || '新录音',
      summary: '（转写结果为空）',
      todos: [],
      ideas: [],
      original: '（转写结果为空）',
      provider: 'rules'
    };
  }

  if (DEEPSEEK_API_KEY) {
    try {
      const deepSeek = await polishTranscriptWithDeepSeek(corrected, job);
      if (deepSeek?.memo) {
        const deepSeekPath = transcriptDeepSeekPathForId(job.id);
        const polishedPath = transcriptPolishedPathForId(job.id);
        await fsp.writeFile(deepSeekPath, `${JSON.stringify({
          id: job.id,
          recordingName: job.recordingName,
          model: deepSeek.model,
          settings: deepSeek.settings,
          usage: deepSeek.usage,
          truncated: deepSeek.truncated,
          createdAt: deepSeek.createdAt,
          raw: deepSeek.raw,
          memo: deepSeek.memo
        }, null, 2)}\n`, 'utf8');
        await fsp.writeFile(polishedPath, `${deepSeek.memo.original}\n`, 'utf8');
        return {
          ...deepSeek.memo,
          provider: 'deepseek',
          deepSeek: {
            model: deepSeek.model,
            usage: deepSeek.usage,
            truncated: deepSeek.truncated,
            path: path.relative(DATA_ROOT, deepSeekPath),
            polishedPath: path.relative(DATA_ROOT, polishedPath)
          }
        };
      }
    } catch (error) {
      const modelError = providerStageError(error, 'DeepSeek', 'deepseek');
      console.error(`DeepSeek polish failed for ${job.id || 'unknown'}: ${modelError.message}`);
      throw modelError;
    }
  }

  const sentences = splitTranscriptSentences(corrected);
  const paragraphs = formatTranscriptParagraphs(sentences);
  return {
    title: makeMemoTitle(sentences, job),
    summary: makeTranscriptSummary(sentences),
    todos: [],
    ideas: [],
    original: formatMemoOriginalText(paragraphs.join('\n\n')),
    provider: 'rules'
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

async function writeMemoForJob(job, text) {
  const memoPayload = await buildFlomoContent(job, text);
  const transcriptMemoPath = transcriptMemoPathForId(job.id);
  await fsp.writeFile(transcriptMemoPath, `${memoPayload.content}\n`, 'utf8');

  const latest = await readJob(job.id);
  latest.phase = 4;
  latest.transcriptMemoPath = path.relative(DATA_ROOT, transcriptMemoPath);
  latest.memo = {
    title: memoPayload.memo.title,
    hasTodos: false,
    hasIdeas: false,
    provider: memoPayload.memo.provider || 'rules'
  };
      if (memoPayload.memo.deepSeek) {
        latest.deepSeek = memoPayload.memo.deepSeek;
        latest.polishedTranscriptPath = memoPayload.memo.deepSeek.polishedPath;
      }
      if (job.autoSpeakerCommand) {
        latest.autoSpeakerCommand = job.autoSpeakerCommand;
        latest.autoSpeakerRetryDone = job.autoSpeakerRetryDone;
        latest.asrSpeakerCountRequested = job.asrSpeakerCountRequested;
        latest.asrSpeakerCount = job.asrSpeakerCount;
      }
      latest.memoUpdatedAt = new Date().toISOString();
  delete latest.pendingReason;
  delete latest.lastError;
  await writeJob(latest);
  return {
    job: latest,
    content: memoPayload.content,
    memo: memoPayload.memo,
    transcriptMemoPath
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

    const requestedAsrSource = normalizeAsrSource(job.asrSourceRequested);
    const asrSource = requestedAsrSource || DEFAULT_ASR_AUDIO_SOURCE;
    const speakerCount = normalizeSpeakerCount(job.asrSpeakerCountRequested ?? DEFAULT_ASR_SPEAKER_COUNT);
    let audioUrl = publicAudioUrl(job.recordingName);
    if (asrSource === 'clean-for-asr') {
      const { meta: asrCleanMeta } = await writeAsrCleanForJob(job, job.asrCleanParams);
      job.asrCleanPath = path.relative(DATA_ROOT, asrCleanPathForId(id));
      job.asrCleanMetaPath = path.relative(DATA_ROOT, asrCleanMetaPathForId(id));
      job.asrClean = {
        createdAt: asrCleanMeta.createdAt,
        params: asrCleanMeta.params,
        metrics: asrCleanMeta.metrics
      };
      job.asrDecision = {
        finalSource: 'clean-for-asr',
        cleanRole: 'manual-final',
        reason: '用户手动选择用识别版重跑转写。',
        updatedAt: new Date().toISOString()
      };
      await writeJob(job);
      audioUrl = publicAsrAudioUrl(id);
    } else if (shouldGenerateAsrCleanComparison(asrSource, requestedAsrSource)) {
      job = await attachAsrCleanForComparison(
        job,
        '自动上传默认使用原始音频转写；识别版只生成给后台试听和调试，避免降噪误伤文字。'
      );
    } else {
      job.asrDecision = {
        finalSource: 'raw',
        cleanRole: 'manual-raw-final',
        reason: '用户手动选择用原始音频重跑转写。',
        updatedAt: new Date().toISOString()
      };
      await writeJob(job);
    }

    job.status = 'transcribing';
    job.phase = 3;
    job.audioUrl = audioUrl.replace(ASR_FILE_TOKEN, '***');
    job.asrSource = asrSource;
    job.asrSpeakerCount = speakerCount;
    delete job.lastError;
    await writeJob(job);

    const taskId = await submitDashScopeTask(audioUrl, { speakerCount });
    job = await readJob(id);
    job.dashScopeTaskId = taskId;
    await writeJob(job);

    const { output, transcriptionUrl } = await waitForDashScopeTask(taskId);
    const transcription = await downloadTranscription(transcriptionUrl);
    const speakerTranscript = extractSpeakerTranscript(transcription);
    const text = speakerTranscript.text || extractTranscriptText(transcription);
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
      model: DASHSCOPE_MODEL,
      audioSource: asrSource,
      speakerCount,
      speakerDiarization: speakerTranscript.text ? {
        speakerCount: speakerTranscript.speakerCount,
        segmentCount: speakerTranscript.segments.length
      } : null,
      disfluencyRemoval: DASHSCOPE_DISFLUENCY_REMOVAL,
      maxWaitMs: DASHSCOPE_MAX_WAIT_MS,
      usage: output.usage,
      taskMetrics: output.task_metrics
    };
    await writeJob(job);

    const speakerCommand = detectSpeakerCountCommand(text);
    if (
      speakerCommand &&
      speakerCommand.count > 1 &&
      speakerCommand.count !== speakerCount &&
      !job.autoSpeakerRetryDone
    ) {
      job.status = 'uploaded';
      job.phase = Math.max(2, Number(job.phase || 2));
      job.asrSpeakerCountRequested = speakerCommand.count;
      job.autoSpeakerRetryDone = true;
      job.autoSpeakerCommand = {
        count: speakerCommand.count,
        position: speakerCommand.position,
        sample: speakerCommand.sample,
        detectedAt: new Date().toISOString()
      };
      job.pendingReason = `检测到${speakerCommand.count}人对话口令，正在自动重跑分角色转写。`;
      await writeJob(job);
      processingJobs.delete(id);
      scheduleProcessJob(id);
      return;
    }

    if (!FLOMO_WEBHOOK_URL) {
      job.status = 'transcribed';
      job.pendingReason = 'flomo is not configured';
      await writeJob(job);
      return;
    }

    const memoPayload = await writeMemoForJob(job, text);
    job = memoPayload.job;
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
    job.status = 'done';
    job.phase = 4;
    job.flomo = {
      sentAt: new Date().toISOString(),
      memoSlug: result?.memo?.slug
    };
    delete job.pendingReason;
    delete job.lastError;
    await writeJob(job);
  } catch (error) {
    const job = await readJob(id).catch(() => null);
    if (job) {
      job.status = error.stage === 'deepseek'
        ? 'deepseek_failed'
        : (job.status === 'transcribing' ? 'transcribe_failed' : 'flomo_failed');
      job.lastError = error.message;
      await writeJob(job).catch(() => {});
      updateDeviceStatus(job.deviceId || 'unknown', {
        lastStatus: job.status,
        lastRecordingName: job.recordingName,
        lastJobId: job.id,
        lastError: job.lastError
      });
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

  const startedAt = new Date().toISOString();
  const deviceId = rawDeviceId.replace(/[^\w.-]/g, '_').slice(0, 80);
  const { id: jobId, recordingName, sourceRecordingName } = uploadIdentityFor(normalized, deviceId, rawRecordedAt, startedAt);
  const uploadPath = path.join(UPLOAD_DIR, recordingName);
  const tempUploadPath = path.join(UPLOAD_DIR, `.${recordingName}.${Date.now()}.${crypto.randomBytes(4).toString('hex')}.tmp`);
  const jobPath = path.join(JOB_DIR, `${jobId}.json`);
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
  if (jobExists && uploadExists) {
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
  if (uploadExists || jobExists) {
    updateDeviceStatus(deviceId, {
      lastStatus: uploadExists ? 'retrying_stale_upload' : 'repairing_missing_upload',
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
      await fsp.writeFile(tempUploadPath, wavBuffer, { flag: 'wx' });
    } else {
      out = fs.createWriteStream(tempUploadPath, { flags: 'wx' });
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

    await fsp.rm(uploadPath, { force: true }).catch(() => {});
    await fsp.rename(tempUploadPath, uploadPath);

    const job = {
      id: jobId,
      status: 'uploaded',
      phase: 1,
      deviceId,
      recordingName,
      sourceRecordingName,
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
    await fsp.rm(tempUploadPath, { force: true }).catch(() => {});
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

async function handleChatUpload(req, res) {
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

  const startedAt = new Date().toISOString();
  const rawDeviceId = String(req.headers['x-device-id'] || 'cardputer-chat').trim();
  const deviceId = rawDeviceId.replace(/[^\w.-]/g, '_').slice(0, 80) || 'cardputer-chat';
  const rawRecordingName = String(req.headers['x-recording-name'] || 'CHAT.wav').trim();
  const normalized = normalizeRecordingName(rawRecordingName);
  if (!normalized) {
    sendJson(res, 400, { ok: false, error: 'invalid recording name' });
    return;
  }

  const id = `${compactTimestampForIdentity(req.headers['x-recorded-at'], new Date(startedAt))}_${slugForIdentity(deviceId, 'device', 40)}_chat_${crypto.randomBytes(3).toString('hex')}`.slice(0, 160);
  const recordingName = `${id}.wav`;
  const audioPath = path.join(CHAT_AUDIO_DIR, recordingName);
  const tempAudioPath = path.join(CHAT_AUDIO_DIR, `.${recordingName}.${Date.now()}.${crypto.randomBytes(4).toString('hex')}.tmp`);
  const jobPath = chatJobPathForId(id);

  let bytes = 0;
  let uploadedBytes = 0;
  let out = null;
  const job = {
    id,
    type: 'chat',
    status: 'uploading',
    phase: 1,
    deviceId,
    recordingName,
    sourceRecordingName: normalized.recordingName,
    recordedAt: String(req.headers['x-recorded-at'] || '').slice(0, 40),
    uploadEncoding: isAdpcmUpload ? 'ima-adpcm' : 'wav',
    createdAt: startedAt,
    updatedAt: startedAt
  };

  try {
    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');

    if (isAdpcmUpload) {
      const encoded = await collectRequestBody(req, MAX_UPLOAD_BYTES, (received) => {
        uploadedBytes = received;
      });
      const pcmBytes = parseHeaderInt(req.headers['x-adpcm-pcm-bytes']);
      const wavBuffer = decodeImaAdpcmToWav(encoded, {
        sampleRate: parseHeaderInt(req.headers['x-adpcm-sample-rate']) || 16000,
        pcmBytes,
        predictor: parseHeaderInt(req.headers['x-adpcm-initial-predictor']),
        index: parseHeaderInt(req.headers['x-adpcm-initial-index']) || 0
      });
      bytes = wavBuffer.length;
      await fsp.writeFile(tempAudioPath, wavBuffer, { flag: 'wx' });
    } else {
      out = fs.createWriteStream(tempAudioPath, { flags: 'wx' });
      req.on('data', (chunk) => {
        bytes += chunk.length;
        uploadedBytes = bytes;
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

    await fsp.rename(tempAudioPath, audioPath);
    Object.assign(job, {
      status: 'uploaded',
      phase: 2,
      bytes,
      uploadedBytes: uploadedBytes || bytes,
      originalBytes: bytes,
      compressionRatio: isAdpcmUpload && uploadedBytes ? Number((bytes / uploadedBytes).toFixed(2)) : 1,
      audioPath: path.relative(DATA_ROOT, audioPath),
      updatedAt: new Date().toISOString()
    });
    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');

    if (!DASHSCOPE_API_KEY || !PUBLIC_BASE_URL || !ASR_FILE_TOKEN) {
      Object.assign(job, {
        status: 'uploaded',
        pendingReason: 'chat transcription is not configured',
        updatedAt: new Date().toISOString()
      });
      await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');
      sendJson(res, 202, {
        ok: true,
        id,
        status: job.status,
        pendingReason: job.pendingReason,
        replyText: '服务器还没有配置语音识别，CHAT 音频已保存。'
      });
      return;
    }

    job.status = 'transcribing';
    job.phase = 3;
    job.audioUrl = publicChatAudioUrl(recordingName).replace(ASR_FILE_TOKEN, '***');
    job.updatedAt = new Date().toISOString();
    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');

    const asr = await transcribePublicAudioUrl(publicChatAudioUrl(recordingName), { speakerCount: 0 });
    const userText = normalizeTranscriptText(await applyProjectTermCorrections(asr.text));
    const transcriptPath = chatTranscriptPathForId(id);
    const transcriptJsonPath = path.join(CHAT_TRANSCRIPT_DIR, `${id}.json`);
    await fsp.writeFile(transcriptPath, `${userText}\n`, 'utf8');
    await fsp.writeFile(transcriptJsonPath, `${JSON.stringify(asr.transcription, null, 2)}\n`, 'utf8');

    let chatReply = {
      replyText: fallbackChatReplyText(userText),
      command: null,
      provider: 'rules'
    };
    if (CHAT_REPLY_ENABLED) {
      try {
        job.chatContext = await recentChatContextForJob(id);
        chatReply = await replyToChatWithDeepSeek(userText, job);
        const deepSeekPath = chatDeepSeekPathForId(id);
        await fsp.writeFile(deepSeekPath, `${JSON.stringify(chatReply, null, 2)}\n`, 'utf8');
        job.chatDeepSeekPath = path.relative(DATA_ROOT, deepSeekPath);
      } catch (error) {
        const modelError = providerStageError(error, 'DeepSeek', 'chat_reply');
        Object.assign(job, {
          status: 'chat_reply_failed',
          phase: 3,
          transcriptPath: path.relative(DATA_ROOT, transcriptPath),
          transcriptJsonPath: path.relative(DATA_ROOT, transcriptJsonPath),
          userText,
          chatReplyError: modelError.message,
          lastError: modelError.message,
          dashScope: {
            taskId: asr.taskId,
            model: DASHSCOPE_MODEL,
            disfluencyRemoval: DASHSCOPE_DISFLUENCY_REMOVAL,
            usage: asr.output.usage,
            taskMetrics: asr.output.task_metrics
          },
          updatedAt: new Date().toISOString()
        });
        await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');
        sendJson(res, 502, {
          ok: false,
          id,
          status: job.status,
          userText,
          error: modelError.message
        });
        return;
      }
    }
    const replyText = String(chatReply.replyText || fallbackChatReplyText(userText)).slice(0, 160);
    const displayText = String(chatReply.displayText || replyText).slice(0, 360);
    const speakText = String(chatReply.speakText || replyText).slice(0, 160);
    let tts = null;
    if (CHAT_TTS_ENABLED && speakText) {
      try {
        tts = await synthesizeChatReplyWav(speakText, id);
      } catch (error) {
        job.chatTtsError = error.message;
      }
    }
    Object.assign(job, {
      status: 'done',
      phase: 4,
      transcriptPath: path.relative(DATA_ROOT, transcriptPath),
      transcriptJsonPath: path.relative(DATA_ROOT, transcriptJsonPath),
      userText,
      replyText,
      displayText,
      speakText,
      replyMode: chatReply.provider === 'deepseek' ? 'chat-reply' : 'asr-fallback',
      command: chatReply.command || null,
      chatReply: {
        provider: chatReply.provider || 'rules',
        model: chatReply.model || '',
        usage: chatReply.usage || null,
        error: chatReply.error || ''
      },
      chatTts: tts,
      dashScope: {
        taskId: asr.taskId,
        model: DASHSCOPE_MODEL,
        disfluencyRemoval: DASHSCOPE_DISFLUENCY_REMOVAL,
        usage: asr.output.usage,
        taskMetrics: asr.output.task_metrics
      },
      updatedAt: new Date().toISOString()
    });
    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8');

    sendJson(res, 201, {
      ok: true,
      id,
      userText,
      replyText: displayText,
      replyMode: job.replyMode,
      command: job.command || null,
      audioUrl: tts?.audioUrl || null
    });
  } catch (error) {
    if (out) out.destroy();
    await fsp.rm(tempAudioPath, { force: true }).catch(() => {});
    Object.assign(job, {
      status: 'failed',
      lastError: error.message,
      updatedAt: new Date().toISOString()
    });
    await fsp.writeFile(jobPath, `${JSON.stringify(job, null, 2)}\n`, 'utf8').catch(() => {});
    sendJson(res, 500, { ok: false, id, error: error.message });
  }
}

async function listChatJobs({ limit = 20 } = {}) {
  const entries = await fsp.readdir(CHAT_JOB_DIR, { withFileTypes: true }).catch(() => []);
  const jobs = [];
  await Promise.all(entries.map(async (entry) => {
    if (!entry.isFile() || !entry.name.endsWith('.json')) return;
    try {
      const raw = await fsp.readFile(path.join(CHAT_JOB_DIR, entry.name), 'utf8');
      const job = JSON.parse(raw);
      jobs.push({
        id: job.id,
        status: job.status,
        deviceId: job.deviceId,
        recordingName: job.recordingName,
        userText: job.userText || '',
        replyText: job.replyText || '',
        replyMode: job.replyMode || '',
        command: job.command || null,
        pendingReason: job.pendingReason || '',
        lastError: job.lastError || '',
        createdAt: job.createdAt,
        updatedAt: job.updatedAt,
        bytes: job.bytes,
        audioPath: job.audioPath
      });
    } catch (error) {
      jobs.push({
        id: entry.name.replace(/\.json$/i, ''),
        status: 'read_failed',
        lastError: error.message
      });
    }
  }));
  jobs.sort((a, b) => String(b.updatedAt || b.createdAt || '').localeCompare(String(a.updatedAt || a.createdAt || '')));
  return jobs.slice(0, Math.max(1, Math.min(100, Number(limit) || 20)));
}

async function handleChatRecentApi(req, res, url) {
  if (!dashboardAuth(req, res)) return;
  const limit = Math.max(1, Math.min(100, parseInt(url.searchParams.get('limit') || '20', 10) || 20));
  try {
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      jobs: await listChatJobs({ limit })
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleChatLatestApi(req, res, url) {
  if (!hasValidChatReadToken(req, url)) {
    sendJson(res, 401, { ok: false, error: 'invalid chat read token' });
    return;
  }
  try {
    const jobs = await listChatJobs({ limit: 20 });
    const latest = jobs.find((job) => job.userText) || jobs[0] || null;
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      job: latest
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleChatInboxApi(_req, res) {
  if (!CHAT_INBOX_PATH) {
    sendJson(res, 404, { ok: false, error: 'chat inbox is not configured' });
    return;
  }
  try {
    const jobs = await listChatJobs({ limit: 20 });
    const latest = jobs.find((job) => job.userText) || jobs[0] || null;
    sendJson(res, 200, {
      ok: true,
      time: new Date().toISOString(),
      job: latest
    });
  } catch (error) {
    sendJson(res, 500, { ok: false, error: error.message });
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
          deepSeek: Boolean(DEEPSEEK_API_KEY),
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

async function handleDeepSeekSettingsApi(req, res) {
  if (!dashboardAuth(req, res)) return;

  try {
    if (req.method === 'GET') {
      sendJson(res, 200, {
        ok: true,
        time: new Date().toISOString(),
        configured: Boolean(DEEPSEEK_API_KEY),
        settings: await readDeepSeekSettings(),
        defaults: normalizeDeepSeekSettings()
      });
      return;
    }

    if (req.method === 'POST') {
      const payload = await readJsonBody(req, 64 * 1024);
      const settings = await writeDeepSeekSettings(payload.settings || payload);
      sendJson(res, 200, {
        ok: true,
        time: new Date().toISOString(),
        configured: Boolean(DEEPSEEK_API_KEY),
        settings
      });
      return;
    }

    sendJson(res, 405, { ok: false, error: 'method not allowed' });
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
  number('rumbleHighpass', 0, 0.9995);
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

function previewHighpassFrameStats(wavBuffer, wav, p) {
  const statsList = [];
  const rmsValues = [];
  const frameBytes = p.frameSamples * 2;
  const end = wav.dataOffset + wav.dataBytes;
  let prevInput = 0;
  let hpState = 0;

  for (let frameStart = wav.dataOffset; frameStart < end; frameStart += frameBytes) {
    const frameSamples = Math.floor(Math.min(frameBytes, end - frameStart) / 2);
    let sumSq = 0;
    let sumDiff = 0;
    let prev = 0;
    for (let i = 0; i < frameSamples; i++) {
      const raw = wavBuffer.readInt16LE(frameStart + i * 2);
      hpState = raw - prevInput + p.rumbleHighpass * hpState;
      prevInput = raw;
      const sample = hpState * p.gain;
      sumSq += sample * sample;
      if (i > 0) sumDiff += Math.abs(sample - prev);
      prev = sample;
    }
    const stats = {
      rms: Math.sqrt(sumSq / Math.max(1, frameSamples)),
      avgDiff: frameSamples > 1 ? sumDiff / (frameSamples - 1) : 0
    };
    statsList.push(stats);
    if (stats.rms > 0) rmsValues.push(stats.rms);
  }

  rmsValues.sort((a, b) => a - b);
  const noiseFloorRms = rmsValues.length
    ? rmsValues[Math.min(rmsValues.length - 1, Math.floor(rmsValues.length * 0.2))]
    : 0;
  const adaptiveNoiseRms = p.noiseRmsMax >= 400 && noiseFloorRms > 0
    ? Math.max(p.noiseRmsMax, Math.min(5000, noiseFloorRms * 1.6 + 80))
    : p.noiseRmsMax;
  return { statsList, noiseFloorRms, adaptiveNoiseRms };
}

function previewModeProfile(mode) {
  const normalized = normalizePreviewMode(mode);
  const profiles = {
    light: { voiceBandMix: 0.994, bodyLowpass: 224, extraLowpass: 0, finalGain: 1.02, humLowpass: 10, humMix: 0.3, machineNoiseScale: 5.0, speechHumRatio: 0.08, speechNoiseRatio: 0.28, speechHumTrackRatio: 0.05, speechBodyProtect: 1.35, spectralStrength: 0.28, spectralFloor: 0.22 },
    heavy: { voiceBandMix: 0.988, bodyLowpass: 224, extraLowpass: 0, finalGain: 1.04, humLowpass: 14, humMix: 0.66, machineNoiseScale: 8.2, speechHumRatio: 0.04, speechNoiseRatio: 0.2, speechHumTrackRatio: 0.02, speechBodyProtect: 1.55, spectralStrength: 0.65, spectralFloor: 0.16 },
    strong: { voiceBandMix: 0.982, bodyLowpass: 224, extraLowpass: 0, finalGain: 1.06, humLowpass: 18, humMix: 0.94, machineNoiseScale: 11.6, speechHumRatio: 0.02, speechNoiseRatio: 0.16, speechHumTrackRatio: 0.008, speechBodyProtect: 1.75, spectralStrength: 1.05, spectralFloor: 0.1 }
  };
  return { mode: normalized, ...profiles[normalized] };
}

function percentile(values, ratio) {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * ratio))];
}

function analyzeMachineNoiseReference(wavBuffer, p) {
  if (!wavBuffer) return null;
  try {
    const wav = parsePcm16Wav(wavBuffer);
    const analysis = previewHighpassFrameStats(wavBuffer, wav, p);
    const rmsValues = analysis.statsList.map((stats) => stats.rms).filter((value) => value > 0);
    const diffValues = analysis.statsList.map((stats) => stats.avgDiff).filter((value) => value >= 0);
    return {
      id: MACHINE_NOISE_REFERENCE_ID,
      durationSec: wav.dataBytes / 2 / wav.sampleRate,
      highpassRmsP50: percentile(rmsValues, 0.5),
      highpassRmsP90: percentile(rmsValues, 0.9),
      avgDiffP50: percentile(diffValues, 0.5),
      avgDiffP90: percentile(diffValues, 0.9),
      frameCount: analysis.statsList.length
    };
  } catch {
    return null;
  }
}

function fft(real, imag, inverse = false) {
  const n = real.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [real[i], real[j]] = [real[j], real[i]];
      [imag[i], imag[j]] = [imag[j], imag[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const angle = (inverse ? 2 : -2) * Math.PI / len;
    const wLenReal = Math.cos(angle);
    const wLenImag = Math.sin(angle);
    for (let i = 0; i < n; i += len) {
      let wReal = 1;
      let wImag = 0;
      for (let j = 0; j < len / 2; j++) {
        const uReal = real[i + j];
        const uImag = imag[i + j];
        const vReal = real[i + j + len / 2] * wReal - imag[i + j + len / 2] * wImag;
        const vImag = real[i + j + len / 2] * wImag + imag[i + j + len / 2] * wReal;
        real[i + j] = uReal + vReal;
        imag[i + j] = uImag + vImag;
        real[i + j + len / 2] = uReal - vReal;
        imag[i + j + len / 2] = uImag - vImag;
        const nextReal = wReal * wLenReal - wImag * wLenImag;
        wImag = wReal * wLenImag + wImag * wLenReal;
        wReal = nextReal;
      }
    }
  }
  if (inverse) {
    for (let i = 0; i < n; i++) {
      real[i] /= n;
      imag[i] /= n;
    }
  }
}

function hannWindow(size) {
  const window = new Float64Array(size);
  for (let i = 0; i < size; i++) {
    window[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (size - 1));
  }
  return window;
}

function highpassSamplesFromWav(wavBuffer, p) {
  const wav = parsePcm16Wav(wavBuffer);
  const sampleCount = wav.dataBytes / 2;
  const samples = new Float64Array(sampleCount);
  let prevInput = 0;
  let hpState = 0;
  for (let i = 0; i < sampleCount; i++) {
    const raw = wavBuffer.readInt16LE(wav.dataOffset + i * 2);
    hpState = raw - prevInput + p.rumbleHighpass * hpState;
    prevInput = raw;
    samples[i] = hpState * p.gain;
  }
  return samples;
}

function buildNoiseMagnitudeProfile(noiseBuffer, p, fftSize, hopSize, window) {
  if (!noiseBuffer) return null;
  let samples;
  try {
    samples = highpassSamplesFromWav(noiseBuffer, p);
  } catch {
    return null;
  }
  if (samples.length < fftSize) return null;
  const bins = fftSize / 2 + 1;
  const sums = new Float64Array(bins);
  const real = new Float64Array(fftSize);
  const imag = new Float64Array(fftSize);
  let frames = 0;
  for (let start = 0; start + fftSize <= samples.length; start += hopSize) {
    real.fill(0);
    imag.fill(0);
    for (let i = 0; i < fftSize; i++) real[i] = samples[start + i] * window[i];
    fft(real, imag, false);
    for (let k = 0; k < bins; k++) sums[k] += Math.hypot(real[k], imag[k]);
    frames++;
  }
  if (!frames) return null;
  for (let k = 0; k < bins; k++) sums[k] /= frames;
  return { magnitudes: sums, frames };
}

function applySpectralMachineNoiseReduction(inputSamples, noiseBuffer, p, profile) {
  if (!noiseBuffer || profile.spectralStrength <= 0) return inputSamples;
  const fftSize = 1024;
  const hopSize = 256;
  const window = hannWindow(fftSize);
  const noiseProfile = buildNoiseMagnitudeProfile(noiseBuffer, p, fftSize, hopSize, window);
  if (!noiseProfile) return inputSamples;
  const output = new Float64Array(inputSamples.length);
  const weights = new Float64Array(inputSamples.length);
  const real = new Float64Array(fftSize);
  const imag = new Float64Array(fftSize);
  const bins = fftSize / 2 + 1;
  for (let start = 0; start < inputSamples.length; start += hopSize) {
    real.fill(0);
    imag.fill(0);
    for (let i = 0; i < fftSize; i++) {
      const index = start + i;
      real[i] = index < inputSamples.length ? inputSamples[index] * window[i] : 0;
    }
    fft(real, imag, false);
    for (let k = 0; k < bins; k++) {
      const mag = Math.hypot(real[k], imag[k]);
      if (mag <= 1e-9) continue;
      const floorMag = mag * profile.spectralFloor;
      const nextMag = Math.max(floorMag, mag - noiseProfile.magnitudes[k] * profile.spectralStrength);
      const scale = nextMag / mag;
      real[k] *= scale;
      imag[k] *= scale;
      if (k > 0 && k < fftSize / 2) {
        real[fftSize - k] *= scale;
        imag[fftSize - k] *= scale;
      }
    }
    fft(real, imag, true);
    for (let i = 0; i < fftSize; i++) {
      const index = start + i;
      if (index >= inputSamples.length) break;
      const w = window[i];
      output[index] += real[i] * w;
      weights[index] += w * w;
    }
  }
  const result = new Int16Array(inputSamples.length);
  for (let i = 0; i < inputSamples.length; i++) {
    result[i] = clampInt16(Math.round(weights[i] > 1e-9 ? output[i] / weights[i] : inputSamples[i]));
  }
  return result;
}

function buildPlayPreviewWav(wavBuffer, params, mode = 'heavy', machineNoiseBuffer = null) {
  const wav = parsePcm16Wav(wavBuffer);
  const p = normalizePreviewParams(params);
  const profile = previewModeProfile(mode);
  const output = Buffer.alloc(44 + wav.dataBytes);
  writeWavHeaderBuffer(output, wav.sampleRate, wav.dataBytes);
  const analysis = previewHighpassFrameStats(wavBuffer, wav, p);
  const machineNoise = analyzeMachineNoiseReference(machineNoiseBuffer, p);
  const machineAdaptiveNoiseRms = machineNoise
    ? Math.max(analysis.adaptiveNoiseRms, machineNoise.highpassRmsP90 * profile.machineNoiseScale)
    : analysis.adaptiveNoiseRms;

  let lp = wavBuffer.readInt16LE(wav.dataOffset);
  let prevInput = 0;
  let hpState = 0;
  let bodyLp = 0;
  let humLp = 0;
  let hold = 0;
  let detectedFrames = 0;
  let noiseFrames = 0;
  let processedFrames = 0;
  let speechProtectedFrames = 0;
  let scratchWet = 0;
  let noiseWet = 0;
  let extraLp = 0;
  const frameBytes = p.frameSamples * 2;
  const end = wav.dataOffset + wav.dataBytes;
  const processedSamples = new Int16Array(wav.dataBytes / 2);
  let sampleIndex = 0;
  let frameIndex = 0;

  for (let frameStart = wav.dataOffset; frameStart < end; frameStart += frameBytes) {
    const frameSamples = Math.floor(Math.min(frameBytes, end - frameStart) / 2);
    const stats = analysis.statsList[frameIndex] || frameStats(wavBuffer, frameStart, frameSamples);
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

    const speechLike = machineNoise
      ? stats.avgDiff > Math.max(80, machineNoise.avgDiffP90 * 2.2) || stats.rms > Math.max(machineAdaptiveNoiseRms * 0.75, machineNoise.highpassRmsP90 * 5)
      : stats.avgDiff > 120 || stats.rms > machineAdaptiveNoiseRms * 0.75;
    if (speechLike) speechProtectedFrames++;

    const quiet = machineAdaptiveNoiseRms > 0 && stats.rms > 0 && stats.rms < machineAdaptiveNoiseRms;
    if (quiet) noiseFrames++;
    if (active || quiet) processedFrames++;

    const scratchTarget = active ? 1 : 0;
    const quietRatio = quiet ? 1 - (stats.rms / Math.max(1, machineAdaptiveNoiseRms)) : 0;
    const noiseTarget = Math.max(0, Math.min(1, quietRatio)) * (speechLike ? profile.speechNoiseRatio : 1);
    const scratchStep = (scratchTarget > scratchWet ? 0.018 : 0.006);
    const noiseStep = (noiseTarget > noiseWet ? 0.006 : 0.002);

    for (let i = 0; i < frameSamples; i++) {
      const raw = wavBuffer.readInt16LE(frameStart + i * 2);
      hpState = raw - prevInput + p.rumbleHighpass * hpState;
      prevInput = raw;
      let sample = clampInt16(Math.round(hpState * p.gain));
      const humTrack = profile.humLowpass * (speechLike ? profile.speechHumTrackRatio : 1);
      humLp += Math.round((sample - humLp) * humTrack / 256);
      const humMix = profile.humMix * (speechLike ? profile.speechHumRatio : 1);
      sample = clampInt16(Math.round(sample - humLp * humMix));
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
        const bodyScale = speechLike ? profile.speechBodyProtect : 3;
        const body = machineAdaptiveNoiseRms > 0
          ? Math.min(1, absSample / Math.max(1, machineAdaptiveNoiseRms * bodyScale))
          : 1;
        const protection = body * body;
        const attenuation = p.noiseMix + (1 - p.noiseMix) * protection;
        const smoothAttenuation = 1 - noiseWet * (1 - attenuation);
        sample = clampInt16(Math.round(sample * smoothAttenuation));
      }

      bodyLp += Math.round((sample - bodyLp) * profile.bodyLowpass / 256);
      sample = clampInt16(Math.round(bodyLp + (sample - bodyLp) * profile.voiceBandMix));
      if (profile.extraLowpass > 0) {
        extraLp += Math.round((sample - extraLp) * profile.extraLowpass / 256);
        sample = extraLp;
      }
      sample = clampInt16(Math.round(sample * profile.finalGain));

      processedSamples[sampleIndex++] = sample;
    }
    frameIndex++;
  }
  const finalSamples = applySpectralMachineNoiseReduction(processedSamples, machineNoiseBuffer, p, profile);
  let outOffset = 44;
  for (let i = 0; i < finalSamples.length; i++) {
    output.writeInt16LE(finalSamples[i], outOffset);
    outOffset += 2;
  }

  return {
    buffer: output,
    params: p,
    metrics: {
      detectedFrames,
      noiseFrames,
      processedFrames,
      speechProtectedFrames,
      totalFrames: Math.ceil((wav.dataBytes / 2) / p.frameSamples),
      durationSec: wav.dataBytes / 2 / wav.sampleRate,
      sampleRate: wav.sampleRate,
      noiseFloorRms: Math.round(analysis.noiseFloorRms),
      adaptiveNoiseRms: Math.round(analysis.adaptiveNoiseRms),
      machineAdaptiveNoiseRms: Math.round(machineAdaptiveNoiseRms),
      machineNoiseReferenceId: machineNoise?.id || null,
      machineNoiseRmsP50: machineNoise ? Math.round(machineNoise.highpassRmsP50) : null,
      machineNoiseRmsP90: machineNoise ? Math.round(machineNoise.highpassRmsP90) : null,
      machineNoiseAvgDiffP90: machineNoise ? Math.round(machineNoise.avgDiffP90) : null,
      analysis: 'post-highpass',
      previewMode: profile.mode,
      voiceBandMix: profile.voiceBandMix,
      bodyLowpass: profile.bodyLowpass,
      extraLowpass: profile.extraLowpass,
      humLowpass: profile.humLowpass,
      humMix: profile.humMix,
      speechHumRatio: profile.speechHumRatio,
      speechNoiseRatio: profile.speechNoiseRatio,
      speechHumTrackRatio: profile.speechHumTrackRatio,
      speechBodyProtect: profile.speechBodyProtect,
      spectralStrength: profile.spectralStrength,
      spectralFloor: profile.spectralFloor,
      algorithm: PLAY_PREVIEW_ALGORITHM
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
  const frameStatsList = [];
  const rmsValues = [];
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
    frameStatsList.push(stats);
    if (stats.rms > 0) rmsValues.push(stats.rms);
  }

  const sortedRms = [...rmsValues].sort((a, b) => a - b);
  const noiseFloorRms = sortedRms.length
    ? sortedRms[Math.min(sortedRms.length - 1, Math.floor(sortedRms.length * 0.2))]
    : 0;
  const adaptiveGateRms = p.noiseGateRms > 0
    ? Math.max(p.noiseGateRms, Math.min(1800, noiseFloorRms * 2.25 + 80))
    : 0;

  const targetGains = frameStatsList.map((stats) => {
    const gateGain = adaptiveGateRms > 0 && stats.rms < adaptiveGateRms
      ? p.noiseGateFloor + (1 - p.noiseGateFloor) * Math.pow(stats.rms / Math.max(1, adaptiveGateRms), 2)
      : 1;
    if (gateGain < 0.999) gatedFrames++;
    return gateGain;
  });

  let smoothedFrameGain = 1;
  for (const target of targetGains) {
    const coeff = target > smoothedFrameGain ? 0.78 : 0.18;
    smoothedFrameGain += (target - smoothedFrameGain) * coeff;
    frameGains.push(smoothedFrameGain);
  }
  for (let i = frameGains.length - 2; i >= 0; i--) {
    frameGains[i] = Math.max(frameGains[i], frameGains[i + 1] * 0.96);
  }

  let frameIndex = 0;
  let samplesInFrame = 0;
  let sampleGate = frameGains[0] ?? 1;
  let minGateGain = 1;
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
    const targetGate = frameGains[frameIndex] ?? 1;
    sampleGate += (targetGate - sampleGate) * (targetGate > sampleGate ? 0.04 : 0.0015);
    minGateGain = Math.min(minGateGain, sampleGate);
    sample *= p.gain * sampleGate;

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
      noiseFloorRms: Math.round(noiseFloorRms),
      adaptiveGateRms: Math.round(adaptiveGateRms),
      minGateGain: Number(minGateGain.toFixed(3)),
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
    polishedTranscript: await statOptionalDataFile(job.polishedTranscriptPath),
    deepSeek: await statOptionalDataFile(job.deepSeek?.path),
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
    const polishedTranscriptText = await readOptionalDataText(job.polishedTranscriptPath);
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
      polishedTranscriptText,
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
    const mode = normalizePreviewMode(payload.mode);
    const job = await readJob(id);
    const sourcePath = audioPathForJob(job);
    const source = await fsp.readFile(sourcePath);
    const machineNoise = await fsp.readFile(machineNoiseReferencePath()).catch(() => null);
    const preview = buildPlayPreviewWav(source, payload.params, mode, machineNoise);
    const previewPath = previewPathForId(id, mode);
    const metaPath = previewMetaPathForId(id, mode);
    const meta = {
      id,
      mode,
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
    const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
    const mode = normalizePreviewMode(url.searchParams.get('mode'));
    const previewPath = previewPathForId(id, mode);
    const previewMeta = await readOptionalDataJson(path.relative(DATA_ROOT, previewMetaPathForId(id, mode)));
    if (previewMeta?.metrics?.algorithm !== PLAY_PREVIEW_ALGORITHM) {
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

async function handleChatAudio(req, res, url) {
  const token = url.searchParams.get('token') || '';
  if (!ASR_FILE_TOKEN || !timingSafeEqualText(token, ASR_FILE_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid audio token' });
    return;
  }

  const recordingName = path.basename(decodeURIComponent(url.pathname.slice('/chat-audio/'.length))).replace(/[^\w.-]/g, '_');
  if (!recordingName || !recordingName.toLowerCase().endsWith('.wav')) {
    sendJson(res, 400, { ok: false, error: 'invalid audio name' });
    return;
  }

  const audioPath = path.join(CHAT_AUDIO_DIR, recordingName);
  try {
    const stat = await fsp.stat(audioPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'private, max-age=300'
    });
    fs.createReadStream(audioPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleChatTtsAudio(req, res, url) {
  const token = url.searchParams.get('token') || '';
  if (!ASR_FILE_TOKEN || !timingSafeEqualText(token, ASR_FILE_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid audio token' });
    return;
  }

  const audioName = path.basename(decodeURIComponent(url.pathname.slice('/chat-tts/'.length))).replace(/[^\w.-]/g, '_');
  if (!audioName || !audioName.toLowerCase().endsWith('.wav')) {
    sendJson(res, 400, { ok: false, error: 'invalid audio name' });
    return;
  }

  const audioPath = path.join(CHAT_TTS_DIR, audioName);
  try {
    const stat = await fsp.stat(audioPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'private, max-age=300'
    });
    fs.createReadStream(audioPath).pipe(res);
  } catch (error) {
    if (error.code === 'ENOENT') {
      sendJson(res, 404, { ok: false, error: 'audio not found' });
      return;
    }
    sendJson(res, 500, { ok: false, error: error.message });
  }
}

async function handleChatStreamAudio(req, res, url) {
  const token = url.searchParams.get('token') || '';
  if (!ASR_FILE_TOKEN || !timingSafeEqualText(token, ASR_FILE_TOKEN)) {
    sendJson(res, 401, { ok: false, error: 'invalid audio token' });
    return;
  }

  const recordingName = path.basename(decodeURIComponent(url.pathname.slice('/chat-stream-audio/'.length))).replace(/[^\w.-]/g, '_');
  if (!recordingName || !recordingName.toLowerCase().endsWith('.wav')) {
    sendJson(res, 400, { ok: false, error: 'invalid audio name' });
    return;
  }

  const audioPath = path.join(CHAT_STREAM_AUDIO_DIR, recordingName);
  try {
    const stat = await fsp.stat(audioPath);
    res.writeHead(200, {
      'Content-Type': 'audio/wav',
      'Content-Length': stat.size,
      'Cache-Control': 'private, max-age=300'
    });
    fs.createReadStream(audioPath).pipe(res);
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
  const url = new URL(req.url, 'http://localhost');
  const requestedAsrSource = normalizeAsrSource(url.searchParams.get('asrSource') || url.searchParams.get('source'));
  const requestedSpeakerCount = normalizeSpeakerCount(url.searchParams.get('speakerCount') || url.searchParams.get('speakers'));
  job.status = 'uploaded';
  job.phase = Math.max(2, Number(job.phase || 2));
  job.reprocessRequestedAt = new Date().toISOString();
  if (requestedAsrSource) {
    job.asrSourceRequested = requestedAsrSource;
  } else {
    delete job.asrSourceRequested;
  }
  if (requestedSpeakerCount > 1) {
    job.asrSpeakerCountRequested = requestedSpeakerCount;
  } else {
    delete job.asrSpeakerCountRequested;
  }
  delete job.pendingReason;
  delete job.lastError;
  await writeJob(job);
  scheduleProcessJob(id);
  sendJson(res, 202, { ok: true, id, status: 'queued' });
}

async function readTranscriptTextForJob(job) {
  let text = job.transcriptText || '';
  if (!text && job.transcriptPath) {
    text = await fsp.readFile(path.join(DATA_ROOT, job.transcriptPath), 'utf8');
  }
  return String(text || '').trim();
}

async function handleJobPolish(req, res, pathname) {
  if (!UPLOAD_TOKEN) {
    sendJson(res, 500, { ok: false, error: 'UPLOAD_TOKEN is not configured' });
    return;
  }
  if (!hasValidUploadToken(req)) {
    sendJson(res, 401, { ok: false, error: 'invalid upload token' });
    return;
  }

  const id = decodeURIComponent(pathname.slice('/jobs/'.length, -'/polish'.length));
  if (!/^[\w.-]+$/.test(id)) {
    sendJson(res, 400, { ok: false, error: 'invalid job id' });
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

  const text = await readTranscriptTextForJob(job);
  if (!text) {
    sendJson(res, 409, { ok: false, error: 'job has no transcript text to polish' });
    return;
  }

  job.status = 'polishing';
  job.polishRequestedAt = new Date().toISOString();
  delete job.pendingReason;
  delete job.lastError;
  await writeJob(job);

  try {
    const result = await writeMemoForJob(job, text);
    const latest = result.job;
    latest.status = 'transcribed';
    latest.pendingReason = 'memo regenerated; flomo not resent';
    await writeJob(latest);
    sendJson(res, 200, {
      ok: true,
      id,
      status: latest.status,
      memo: latest.memo,
      transcriptMemoPath: latest.transcriptMemoPath,
      polishedTranscriptPath: latest.polishedTranscriptPath
    });
  } catch (error) {
    const latest = await readJob(id).catch(() => job);
    latest.status = 'polish_failed';
    latest.lastError = error.message;
    await writeJob(latest).catch(() => {});
    sendJson(res, 500, { ok: false, error: error.message });
  }
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
  const text = await readTranscriptTextForJob(job);
  if (!text) {
    sendJson(res, 409, { ok: false, error: 'job has no transcript text to resend' });
    return;
  }

  const memoPayload = await writeMemoForJob(job, text);
  const flomoPayload = await fetchJson(FLOMO_WEBHOOK_URL, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({ content: memoPayload.content })
  });
  if (typeof flomoPayload?.code === 'number' && flomoPayload.code !== 0) {
    throw new Error(flomoPayload?.message || `flomo returned code ${flomoPayload.code}`);
  }

  job = memoPayload.job;
  job.status = 'done';
  job.phase = 4;
  job.flomo = {
    ...(job.flomo || {}),
    resentAt: new Date().toISOString(),
    memoSlug: flomoPayload?.memo?.slug || job.flomo?.memoSlug
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
  if (req.method === 'GET' && CHAT_INBOX_PATH && pathname === CHAT_INBOX_PATH) {
    await handleChatInboxApi(req, res);
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
  if ((req.method === 'GET' || req.method === 'POST') && pathname === '/api/deepseek-settings') {
    await handleDeepSeekSettingsApi(req, res);
    return;
  }
  if (req.method === 'GET' && pathname === '/api/chat/recent') {
    await handleChatRecentApi(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname === '/api/chat/latest') {
    await handleChatLatestApi(req, res, url);
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
  if (req.method === 'POST' && pathname === '/chat/upload') {
    await handleChatUpload(req, res);
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
  if (req.method === 'GET' && pathname.startsWith('/chat-audio/')) {
    await handleChatAudio(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/chat-tts/')) {
    await handleChatTtsAudio(req, res, url);
    return;
  }
  if (req.method === 'GET' && pathname.startsWith('/chat-stream-audio/')) {
    await handleChatStreamAudio(req, res, url);
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
  if (req.method === 'POST' && pathname.startsWith('/jobs/') && pathname.endsWith('/polish')) {
    await handleJobPolish(req, res, pathname);
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

async function runChatReplyProbe() {
  const probes = (process.env.CHAT_REPLY_PROBE_TEXTS || [
    '给我画一只小猪',
    '这只小猪好丑',
    '给我换一只小猪',
    '为什么你刚才一直像在复读',
    '我想用两句话介绍一下这个小机器',
    '算一下 17 乘以 23'
  ].join('\n')).split(/\r?\n/).map((item) => item.trim()).filter(Boolean);
  const context = [
    {
      userText: '给我画一只小猪',
      replyText: '画好了。'
    }
  ];
  for (const text of probes) {
    const reply = await replyToChatWithDeepSeek(text, {
      id: `probe_${Date.now()}`,
      deviceId: 'probe',
      chatContext: context
    });
    console.log(JSON.stringify({
      userText: text,
      replyText: reply.replyText,
      displayText: reply.displayText,
      speakText: reply.speakText,
      provider: reply.provider,
      model: reply.model || ''
    }, null, 2));
    context.push({ userText: text, replyText: reply.replyText });
    if (context.length > 4) context.shift();
  }
}

async function main() {
  await ensureRuntimeDirs();
  await loadChatTtsSettings();

  if (/^(1|true|yes)$/i.test(process.env.CHAT_REPLY_PROBE || '')) {
    await runChatReplyProbe();
    return;
  }

  const server = http.createServer((req, res) => {
    route(req, res).catch((error) => {
      sendJson(res, 500, { ok: false, error: error.message });
    });
  });
  attachChatStream(server);

  server.listen(PORT, () => {
    console.log(`Cardputer cloud voice server listening on ${PORT}`);
    console.log(`Data root: ${DATA_ROOT}`);
  });
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
