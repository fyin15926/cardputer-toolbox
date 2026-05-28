/*
 * 我的工具箱 (My Toolbox) for M5Stack Cardputer-Adv
 * 风格: 纯黑背景 + 黑客帝国荧光绿 + 线性 + 中文界面 + 横屏(240x135)
 *
 * 工具箱交互:
 *   开机: 自动开始录音; 息屏: 空格=录音, 回车=录音列表, F=番茄钟, Alt=应用列表
 *
 * 录音应用交互:
 *   录制中: 空格=暂停/继续; 回车=结束(存盘并进入列表); Esc=存盘并息屏; 长按Del 1.2秒=取消并删除本条; W/S=音量
 *   录音列表: 左/右切 REC/KEY/IMP; ;/. 上下选; 回车=播放; Esc=息屏; 长按Del=删除; Tab+键=绑定快捷; Tab+Enter=标重要; Alt=降噪
 *   回放: 播放完自动回列表; 回车=暂停/继续; 退格=回列表; ;/.=上一条/下一条; Esc=息屏; +/- (= 与 - 键)=音量; 长按Del 1.2秒=删除当前录音; 空格=去录音
 *   绑定的播放键=最高优先级, 任意界面(录音中除外)即按即播, 且播放中按别的键可覆盖切换
 *
 * 方向键(物理): ; = 上, . = 下, , = 左, / = 右
 */
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include "esp_sleep.h"   // 轻睡眠(息屏省电)
#include "driver/gpio.h" // 键盘中断唤醒(GPIO11)

#define UPLOAD_WIFI_ENABLED 1  // 需要使用 8MB Flash + 3MB APP 分区编译
#if UPLOAD_WIFI_ENABLED
#include <WiFi.h>
#include "esp_wifi.h"
#include <time.h>
#endif

// ---------- 屏幕方向 ----------
// 横屏: 1 或 3. 若上下颠倒, 改另一个即可.
#define SCREEN_ROT 1

// ---------- 配色 ----------
#define COL_BG    0x0000   // 纯黑
#define COL_GREEN 0x07E0   // 荧光绿(亮)
#define COL_DIM   0x0320   // 暗绿
#define COL_RED   0xF800   // 录音红点
#define COL_WHITE 0xFFFF   // 播放头白线
#define COL_GRAY  0x7BEF
#define COL_DARK_GRAY 0x2104

// ---------- 全屏 UI 布局常量 ----------
// 屏幕 240×135; 不再绘制右侧标签栏, 所有界面使用全宽内容区
#define CONTENT_W  240     // 内容区宽度 (x: 0..239)
#define WAVE_TOP    20     // 波形区顶 y
#define WAVE_BOT   109     // 波形区底 y  (余 26px 给计时器)
#define WAVE_H      89     // 波形区高度
#define WAVE_CY     50     // A/B 分界: 约 2:1, B线作为主视觉
#define CHROME_TOP  (WAVE_BOT + 1)
#define CHROME_H    (135 - CHROME_TOP)
static const int A_CY   = (WAVE_TOP + WAVE_CY) / 2;        // A线/轨道线中轴
static const int A_HALF = (WAVE_CY - WAVE_TOP) / 2 - 2;    // A线最大半幅
static const int B_CY   = (WAVE_CY + WAVE_BOT) / 2;        // B线/监听线中轴
static const int B_HALF = (WAVE_BOT - WAVE_CY) / 2 - 2;    // B线最大半幅
static const uint8_t REC_B_SMOOTH = 2;   // 录音 B线平滑: 越大越稳, 越小越灵敏
static const uint8_t PB_B_SMOOTH  = REC_B_SMOOTH;   // 播放 B线跟录音页一致
static const uint8_t B_DECAY      = 5;   // 无新音频时回中线速度: 越小回落越快
static const int32_t A_RMS_FULL   = 18000; // A线满格阈值: 越大越不敏感

// ---------- 录音参数 ----------
static const uint8_t REC_B_GAIN   = 5;
static const uint8_t PB_B_GAIN    = 3;
static const uint8_t PB_B_SMOOTH_FAST = 2;

#define FONT_CN_16(g) do { (g).setTextSize(1); (g).setFont(&fonts::efontCN_16); } while (0)
#define FONT_CN_12(g) do { (g).setTextSize(1); (g).setFont(&fonts::efontCN_12); } while (0)
#define FONT_ASCII(g) do { (g).setTextSize(1); (g).setFont(&fonts::AsciiFont8x16); } while (0)
#define FONT_TIMER(g) do { (g).setTextSize(2); (g).setFont(&fonts::Font8x8C64); } while (0)

enum Dseg14Seg : uint16_t {
  DS_A = 1 << 0, DS_B = 1 << 1, DS_C = 1 << 2, DS_D = 1 << 3,
  DS_E = 1 << 4, DS_F = 1 << 5, DS_G = 1 << 6, DS_H = 1 << 7,
  DS_I = 1 << 8, DS_J = 1 << 9, DS_K = 1 << 10, DS_M = 1 << 11,
  DS_N = 1 << 12
};

static const uint32_t REC_RATE = 16000;  // 16kHz
static const int32_t  SHORTCUT_TRIM_RMS = 900;   // 快捷音频切头去尾静音阈值
static const uint32_t SHORTCUT_HEAD_PAD = REC_RATE / 50;  // 20ms, 防止开头被切硬
static const uint32_t SHORTCUT_TAIL_PAD = REC_RATE / 10;  // 100ms, 留一点自然收尾
static const int32_t  SHORTCUT_RETRIM_RMS = 1300; // 再次绑定时更强修边
static const uint32_t SHORTCUT_RETRIM_HEAD_PAD = REC_RATE / 200; // 5ms
static const uint32_t SHORTCUT_RETRIM_TAIL_PAD = REC_RATE / 40;  // 25ms
static const uint32_t SHORTCUT_FADE_SAMPLES = REC_RATE / 125; // 8ms 淡入淡出
static const size_t   REC_N    = 256;    // 每缓冲样本数 (~16ms, 小=波形更流畅)
static const size_t   PB_N     = 256;    // 播放缓冲样本数 (~16ms, B线更贴近录音页)
static const uint32_t REC_UI_FRAME_MS = 40;  // Keep UI below audio/SD priority; 25fps is enough for the small waveform.
static const uint32_t PB_UI_FRAME_MS  = 40;  // Lower display traffic avoids playback stutter and visible flicker.
static const uint32_t REC_AUTO_SEGMENT_MS = 30UL * 60UL * 1000UL;
static const uint8_t  REC_WRITE_BATCH = 4;
static const uint8_t  REC_REARM_SETTLE_BUFFERS = 6;
static const uint32_t UPLOAD_IDLE_DELAY_MS = 3000;
static int16_t recBuf[2][REC_N];
static int16_t pbBuf[2][PB_N];           // 播放双缓冲(放一块/读另一块, 避免覆盖破音)
static int16_t recWriteBuf[REC_WRITE_BATCH * REC_N];
static int16_t recVisualBuf[REC_N];
static int16_t playbackVisualBuf[PB_N];
static int16_t seekToneBuf[384];
int recGain = 36;                        // 录音软件增益默认值(W/S 现场可调, 带削波保护)
int playVol = 200;                       // 回放音量(+/- 可调, 0..255)
static const int VOL_LEVELS = 10;
static const uint8_t BRIGHT_LEVELS = 5;
static const uint8_t BRIGHT_VALUES[BRIGHT_LEVELS] = {25, 55, 90, 125, 170};
static uint8_t brightLevel = 2;
static uint32_t lastUploadTickMs = 0;
static uint32_t lastUploadPendingPollMs = 0;
static const uint32_t UPLOAD_PENDING_POLL_MS = 60000;
static int g_batteryShown = -1;
static int g_batteryCandidate = -1;
static uint8_t g_batteryCandidateHits = 0;
static uint32_t g_batteryLastSampleMs = 0;

#define UPSTAT_IDLE     0
#define UPSTAT_QUEUED   1
#define UPSTAT_DONE     2
#define UPSTAT_NO_CFG   3
#define UPSTAT_BAD_URL  4
#define UPSTAT_WIFI_ERR 5
#define UPSTAT_HTTP_ERR 6
#define UPSTAT_NO_SD    7
#define UPSTAT_NO_FILE  8
#define UPSTAT_ABORTED  9
#define UPSTAT_SYNC_OFF 10
#define UPSTAT_UPLOADING 11
#define UPSTAT_PROCESSING 12
#define UPSTAT_MODEL_ERR 13
#define UPSTAT_JOB_ERR 14
static uint8_t g_uploadStatus = UPSTAT_IDLE;
static bool g_uploadPausedForInput = false;
static bool g_uploadCancelRequested = false;
static int g_uploadActiveRec = 0;
static bool g_uploadActiveAnnounced = false;
static bool g_mediaBusy = false;

// SD 引脚(运行时由 M5Unified 按机型给出, 失败则回退到 Adv 已知值)
int sdSCLK = 40, sdMISO = 39, sdMOSI = 14, sdCS = 12;

static const char *REC_DIR = "/REC";
static const char *SHORTCUT_DIR = "/SHORTCUT";
static const char *IMPORTANT_DIR = "/IMPORTANT";
static const char *CHAT_DIR = "/CHAT";
static const char *CHAT_LAST_PATH = "/CHAT/CHAT_LAST.wav";
static const char *CHAT_REPLY_PATH = "/CHAT/CHAT_REPLY.wav";
static const char *HOTKEY_PATH = "/SHORTCUT/keys.txt";
static const char *OLD_IMPORTANT_HOTKEY_PATH = "/IMPORTANT/keys.txt";
static const char *OLD_HOTKEY_PATH = "/REC/keys.txt";
static const char *NEXT_INDEX_PATH = "/REC/.next";
static const char *REC_ORDER_PATH = "/REC/.order";
static const char *UPLOAD_DIR = "/UPLOAD";
static const char *UPLOAD_QUEUE_PATH = "/UPLOAD/queue.txt";
static const char *UPLOAD_DONE_PATH = "/UPLOAD/done.txt";
static const char *UPLOAD_PENDING_PATH = "/UPLOAD/pending.txt";
static const char *UPLOAD_MODEL_ERR_PATH = "/UPLOAD/model_err.txt";
static const char *UPLOAD_JOB_ERR_PATH = "/UPLOAD/job_err.txt";
static const char *UPLOAD_CONFIG_PATH = "/UPLOAD/net.txt";
static const char *UPLOAD_RECORDED_AT_PATH = "/UPLOAD/recorded_at.txt";
static const char *REC_VOLUME_PATH = "/REC/volume.txt";
static const size_t UPLOAD_CHUNK_BYTES = 4096;
static const uint32_t UPLOAD_ADPCM_THRESHOLD_BYTES = 3UL * 1024UL * 1024UL;
static const uint8_t UPLOAD_BATCH_MAX_JOBS = 40;
static const uint32_t UPLOAD_BATCH_BUDGET_MS = 180000;
static const uint32_t UPLOAD_MODE_HOLD_MS = 850;
static const int MAX_REC = 9999;
static int8_t recVolumeDelta[MAX_REC];
static bool recVolumeDirty = false;
static const uint16_t FRICTION_NOW_SEC = 60;
static const uint16_t FRICTION_IDLE_MAX_SEC = 20 * 60;
static const uint32_t AUTO_SLEEP_MS = 45000;

enum RecKind : uint8_t { REC_NORMAL = 0, REC_SHORTCUT = 1, REC_IMPORTANT = 2 };

static const int8_t IMA_INDEX_TABLE[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};
static const int16_t IMA_STEP_TABLE[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
  19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static void recordingPathKind(int recNum, uint8_t kind, char *p, size_t n) {
  const char *dir = (kind == REC_SHORTCUT) ? SHORTCUT_DIR : ((kind == REC_IMPORTANT) ? IMPORTANT_DIR : REC_DIR);
  snprintf(p, n, "%s/REC_%04d.wav", dir, recNum);
}

static void recordingPath(int recNum, bool important, char *p, size_t n) {
  recordingPathKind(recNum, important ? REC_IMPORTANT : REC_NORMAL, p, n);
}

// ---------- 快捷键绑定 (字母键 -> 录音编号), 存到 SD 卡 /SHORTCUT/keys.txt ----------
struct HotKey { char key; int idx; };
static const int MAX_HOTKEY = 36;
HotKey hotkeys[MAX_HOTKEY];
int hotkeyCount = 0;
static bool hotkeysLoaded = false;

static bool micInputReady = false;
static bool forceMicRearm = true;  // 开机/唤醒后的第一次正式录音要强制重建输入链路
static bool autoRecordPending = true;
static bool speakerOutputReady = false;

#define APP_SLEEP      0
#define APP_REC_RECORD 1
#define APP_REC_LIST   2
#define APP_LAUNCHER   3
#define APP_POMODORO   4
#define APP_WIFI       5
#define APP_CHAT       6

static uint8_t wakeApp = APP_REC_LIST;

// ---------- SD 辅助 ----------
static bool sdMount() {
  SPI.begin(sdSCLK, sdMISO, sdMOSI, sdCS);
  return SD.begin(sdCS, SPI, 25000000);
}

static int parseRecordingNumber(const char *name) {
  if (!name) return 0;
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (strncmp(base, "REC_", 4) != 0) return 0;
  if (base[4] < '0' || base[4] > '9' || base[5] < '0' || base[5] > '9' ||
      base[6] < '0' || base[6] > '9' || base[7] < '0' || base[7] > '9') return 0;
  if (!(base[8] == '.' && (base[9] == 'w' || base[9] == 'W') &&
        (base[10] == 'a' || base[10] == 'A') && (base[11] == 'v' || base[11] == 'V'))) return 0;
  if (base[12] != '\0') return 0;
  return (base[4] - '0') * 1000 + (base[5] - '0') * 100 + (base[6] - '0') * 10 + (base[7] - '0');
}

// ---------- WAV 头 ----------
static void wr32(File &f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  f.write(b, 4);
}
static void wr16(File &f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  f.write(b, 2);
}
static void writeWavHeader(File &f, uint32_t rate, uint32_t dataBytes) {
  f.seek(0);
  f.write((const uint8_t *)"RIFF", 4);
  wr32(f, 36 + dataBytes);
  f.write((const uint8_t *)"WAVE", 4);
  f.write((const uint8_t *)"fmt ", 4);
  wr32(f, 16);
  wr16(f, 1);          // PCM
  wr16(f, 1);          // 单声道
  wr32(f, rate);
  wr32(f, rate * 2);   // byteRate
  wr16(f, 2);          // blockAlign
  wr16(f, 16);         // bits
  f.write((const uint8_t *)"data", 4);
  wr32(f, dataBytes);
}

// ---------- 界面返回码 ----------
#define R_BACK   0   // 退格: 返回上一层
#define R_RECORD 1   // 空格: 去开始录音
#define R_PLAY   2   // 按下某个已绑定的播放键: 立刻去播放(可覆盖当前播放)
#define R_LIST   3   // 列表键: 进入录音列表
#define R_NOISE  4   // Alt: 对最近/当前录音降噪
#define R_DELETE 5   // Del长按确认后删除当前录音
int g_nextPlay = 0;  // 配合 R_PLAY: 要切换去播放的录音编号
int g_afterRecord = R_LIST;  // 录音结束后跳转目标
static bool g_uploadAfterRecord = false;
int g_listReturnRec = 0;     // 播放页返回列表时应重新选中的录音
int g_carryDeleteRec = 0;
uint32_t g_carryDeleteStart = 0;
uint8_t g_listMode = REC_NORMAL;  // 0普通 / 1快捷 / 2重要
static int g_listModeSelectedRec[3] = {0, 0, 0};
static int nextRecHint = 0;  // 下一个录音编号缓存, 避免每次从 REC_0001 顺序探测
static const uint32_t DELETE_HOLD_MS = 1200;
static const uint32_t DELETE_HINT_MS = 280;  // 短按 Del 不显示删除提示, 长按后才出现

// ---------- 按键小工具 ----------
// 取当前按下的第一个可绑定键(字母转小写, 或数字); 没有则返回 0
static char normalizeBindKey(char c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
  switch (c) {
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    default: return 0;
  }
}

static char pressedBindKey() {
  for (char c = '0'; c <= '9'; c++) {
    if (M5Cardputer.Keyboard.isKeyPressed(c)) return c;
  }
  const char shiftedDigits[] = ")!@#$%^&*(";
  for (int i = 0; i < 10; i++) {
    if (M5Cardputer.Keyboard.isKeyPressed(shiftedDigits[i])) return (char)('0' + i);
  }
  for (char c = 'a'; c <= 'z'; c++) {
    if (M5Cardputer.Keyboard.isKeyPressed(c)) return c;
    if (M5Cardputer.Keyboard.isKeyPressed((char)(c - 32))) return c;
  }
  for (char c : M5Cardputer.Keyboard.keysState().word) {
    char normalized = normalizeBindKey(c);
    if (normalized) return normalized;
  }
  return 0;
}
static char dispKey(char k) { return (k >= 'a' && k <= 'z') ? (k - 32) : k; }  // 显示用(字母转大写)
static bool keyCtrl()  { return M5Cardputer.Keyboard.keysState().ctrl; }
static bool keyAlt()   { return M5Cardputer.Keyboard.keysState().alt; }
static bool keyGo()    { return M5Cardputer.BtnA.isPressed(); }
static bool keySpace() { return M5Cardputer.Keyboard.isKeyPressed(' '); }
static bool keyEsc()   { return M5Cardputer.Keyboard.isKeyPressed('`'); }  // Cardputer 物理 Esc 键映射为左上角 `
static bool keyDel()   { return M5Cardputer.Keyboard.keysState().del; }
static bool keyEnter() { return M5Cardputer.Keyboard.keysState().enter; }
static bool keyTab()   { return M5Cardputer.Keyboard.keysState().tab; }
static bool keyUp()    { return M5Cardputer.Keyboard.isKeyPressed(';'); }
static bool keyDown()  { return M5Cardputer.Keyboard.isKeyPressed('.'); }
static bool keyLeft()  { return M5Cardputer.Keyboard.isKeyPressed(','); }
static bool keyRight() { return M5Cardputer.Keyboard.isKeyPressed('/'); }
static bool keyUpload() { return M5Cardputer.Keyboard.isKeyPressed('\\') || M5Cardputer.Keyboard.isKeyPressed('|'); }
static bool keyChat() { return M5Cardputer.Keyboard.isKeyPressed('c') || M5Cardputer.Keyboard.isKeyPressed('C'); }
static bool keyVolUp() { return !keyTab() && (M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+')); }
static bool keyVolDn() { return !keyTab() && (M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_')); }
static bool keyBrightUp() { return !keyTab() && (M5Cardputer.Keyboard.isKeyPressed(']') || M5Cardputer.Keyboard.isKeyPressed('}')); }
static bool keyBrightDn() { return !keyTab() && (M5Cardputer.Keyboard.isKeyPressed('[') || M5Cardputer.Keyboard.isKeyPressed('{')); }
static bool keyUploadAbort() {
  if (!M5Cardputer.Keyboard.isPressed()) return false;
  g_uploadPausedForInput = true;
  if (keyTab() && keyUpload()) g_uploadCancelRequested = true;
  return true;
}

static bool wakeAppFromPressedKeys(uint8_t &app) {
  if (keySpace()) {
    app = APP_REC_RECORD;
    return true;
  }
  if (keyEnter()) {
    app = APP_REC_LIST;
    return true;
  }
  if (M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F')) {
    app = APP_POMODORO;
    return true;
  }
  if (keyChat()) {
    app = APP_CHAT;
    return true;
  }
  if (keyGo()) {
    app = APP_LAUNCHER;
    return true;
  }
  return false;
}

static void adjustPlayVolume(int delta) {
  int level = (playVol * VOL_LEVELS + 127) / 255;
  if (delta > 0) level++;
  else if (delta < 0) level--;
  level = max(0, min(VOL_LEVELS, level));
  playVol = (level * 255 + VOL_LEVELS / 2) / VOL_LEVELS;
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(playVol);
}

static int globalVolumeLevel() {
  int level = (playVol * VOL_LEVELS + 127) / 255;
  return max(0, min(VOL_LEVELS, level));
}

static int volumeFromLevel(int level) {
  level = max(0, min(VOL_LEVELS, level));
  return (level * 255 + VOL_LEVELS / 2) / VOL_LEVELS;
}

static int effectiveVolumeLevelForRec(int recNum) {
  int delta = (recNum > 0 && recNum <= MAX_REC) ? recVolumeDelta[recNum - 1] : 0;
  return max(0, min(VOL_LEVELS, globalVolumeLevel() + delta));
}

static void applyPlaybackVolumeForRec(int recNum) {
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(volumeFromLevel(effectiveVolumeLevelForRec(recNum)));
}

static void applyBrightness() {
  M5Cardputer.Display.setBrightness(BRIGHT_VALUES[brightLevel]);
}

static void adjustBrightness(int delta) {
  int level = (int)brightLevel;
  if (delta > 0) level++;
  else if (delta < 0) level--;
  brightLevel = (uint8_t)max(0, min((int)BRIGHT_LEVELS - 1, level));
  applyBrightness();
}

// 等所有键松开
static void waitRelease() {
  do { M5Cardputer.update(); delay(8); } while (M5Cardputer.Keyboard.isPressed());
}

static bool autoSleepDue(uint32_t lastInputMs, uint32_t now = millis()) {
  return !M5Cardputer.Keyboard.isPressed() && now - lastInputMs >= AUTO_SLEEP_MS;
}

static bool prepareMicInput(bool force = false) {
  if (force && micInputReady) {
    while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    M5Cardputer.Mic.end();
    micInputReady = false;
    delay(12);
  }
  if (micInputReady) return true;
  M5Cardputer.Speaker.end();
  speakerOutputReady = false;
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x13, 0x00, 400000);  // HP drive OFF
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x12, 0xFC, 400000);  // DAC 掉电
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x32, 0x00, 400000);  // DAC→HP 混音器断开
  {
    auto mc = M5Cardputer.Mic.config();
    mc.magnification = 1; mc.noise_filter_level = 3;
    M5Cardputer.Mic.config(mc);
  }
  bool ok = M5Cardputer.Mic.begin();
  delay(12);
  for (uint8_t pga = 0x10; pga <= 0x17; pga++) {
    M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, pga, 400000);
    delay(3);
  }
  delay(8);
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);  // 开机首次偶发低增益, 重写一次确保生效
  micInputReady = ok;
  return ok;
}

static void processMicBuffer(int16_t *buf, size_t n, int32_t &dc, int32_t &hum, int32_t &lpf, int32_t &noiseRms, int32_t &softGateQ8) {
  if (!buf || n == 0) return;

  int64_t sumSq = 0;
  int64_t sumDiff = 0;
  for (size_t k = 0; k < n; k++) {
    int32_t s = buf[k];
    sumSq += (int64_t)s * s;
    if (k > 0) sumDiff += abs((int)(buf[k] - buf[k - 1]));
  }

  int32_t rms = (int32_t)sqrtf((float)((double)sumSq / n));
  int32_t avgDiff = (n > 1) ? (int32_t)(sumDiff / (n - 1)) : 0;
  bool likelyFriction = rms > 0 &&
                        rms < 520 &&
                        avgDiff > 120 &&
                        (int64_t)avgDiff * 256 > (int64_t)rms * 120;
  bool quietEnoughForNoise = rms > 0 && rms < max((int32_t)220, noiseRms * 3) && !likelyFriction;

  if (quietEnoughForNoise) {
    noiseRms = (noiseRms * 31 + rms) >> 5;
    if (noiseRms < 45) noiseRms = 45;
  } else if (noiseRms < 90) {
    noiseRms++;
  }

  int32_t targetGateQ8 = 256;
  if (rms > 0 && rms < noiseRms * 2) {
    targetGateQ8 = 224;
  } else if (rms > 0 && rms < noiseRms * 3) {
    targetGateQ8 = 240;
  }
  if (likelyFriction && targetGateQ8 > 232) targetGateQ8 = 232;
  softGateQ8 += (targetGateQ8 - softGateQ8) >> 3;

  bool voiceLike = rms > max(noiseRms * 4, (int32_t)900) || avgDiff > 90;
  const int32_t hpShift = 7;
  const int32_t humShift = voiceLike ? 11 : 8;
  const int32_t humMixQ8 = voiceLike ? 32 : 144;
  const int32_t lpfCoef = likelyFriction ? 128 : 160;
  const int32_t hiMixQ8 = likelyFriction ? 176 : 256;

  for (size_t k = 0; k < n; k++) {
    int32_t raw = buf[k];
    dc += (raw - dc) >> hpShift;
    int32_t hp = raw - dc;
    hum += (hp - hum) >> humShift;
    int32_t cleaned = hp - ((hum * humMixQ8) >> 8);
    int32_t x = (int32_t)(((int64_t)cleaned * recGain * softGateQ8) >> 8);
    lpf += (int32_t)(((int64_t)(x - lpf) * lpfCoef) >> 8);
    int32_t y = lpf;
    if (hiMixQ8 < 256) {
      int32_t hi = x - lpf;
      y = lpf + ((hi * hiMixQ8) >> 8);
    }
    float yf = 32767.0f * tanhf((float)y / 32767.0f);
    buf[k] = (int16_t)yf;
  }
}

static int8_t calcTrackAmp(const int16_t *buf, size_t n) {
  if (!buf || n == 0) return 0;
  int64_t sum = 0;
  for (size_t k = 0; k < n; k++) sum += buf[k];
  int32_t mean = (int32_t)(sum / (int64_t)n);
  int64_t sumSq = 0;
  for (size_t k = 0; k < n; k++) {
    int32_t centered = (int32_t)buf[k] - mean;
    sumSq += (int64_t)centered * centered;
  }
  float rms = sqrtf((float)((double)sumSq / n));
  const float visualFloor = 1400.0f;
  if (rms <= visualFloor) return 0;
  rms -= visualFloor;
  int sign = (((int32_t)buf[n / 2] - mean) >= 0) ? 1 : -1;
  float norm = rms / (float)(A_RMS_FULL - 1400);
  if (norm > 1.0f) norm = 1.0f;
  int amp = (int)(sqrtf(norm) * A_HALF + 0.5f) * sign;
  if (amp > A_HALF) amp = A_HALF;
  if (amp < -A_HALF) amp = -A_HALF;
  return (int8_t)amp;
}

// ---------- 通用 UI (横屏 240x135) ----------
// 顶栏: 标题(亮绿16) + 左侧小竖条点缀 + 下分隔线
static void drawHeader(const char *title) {
  auto &d = M5Cardputer.Display;
  d.fillRect(2, 5, 3, 13, COL_GREEN);          // 标题前的荧光绿小竖条(点缀)
  FONT_CN_16(d);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(10, 3);
  d.print(title);
  d.drawFastHLine(0, 21, d.width(), COL_DIM);
}

// 电量: 内容区右上角小字
static uint16_t dseg14Mask(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  switch (c) {
    case '0': return DS_A | DS_B | DS_C | DS_D | DS_E | DS_F;
    case '1': return DS_B | DS_C;
    case '2': return DS_A | DS_B | DS_G | DS_E | DS_D;
    case '3': return DS_A | DS_B | DS_C | DS_D | DS_G;
    case '4': return DS_F | DS_G | DS_B | DS_C;
    case '5': return DS_A | DS_F | DS_G | DS_C | DS_D;
    case '6': return DS_A | DS_F | DS_E | DS_D | DS_C | DS_G;
    case '7': return DS_A | DS_B | DS_C;
    case '8': return DS_A | DS_B | DS_C | DS_D | DS_E | DS_F | DS_G;
    case '9': return DS_A | DS_B | DS_C | DS_D | DS_F | DS_G;
    case 'A': return DS_A | DS_B | DS_C | DS_E | DS_F | DS_G;
    case 'B': return DS_F | DS_E | DS_D | DS_C | DS_G;
    case 'C': return DS_A | DS_F | DS_E | DS_D;
    case 'D': return DS_A | DS_B | DS_C | DS_D | DS_E | DS_F;
    case 'E': return DS_A | DS_F | DS_E | DS_D | DS_G;
    case 'F': return DS_A | DS_F | DS_E | DS_G;
    case 'G': return DS_A | DS_F | DS_E | DS_D | DS_C;
    case 'H': return DS_F | DS_E | DS_G | DS_B | DS_C;
    case 'I': return DS_A | DS_D | DS_M | DS_N;
    case 'J': return DS_B | DS_C | DS_D | DS_E;
    case 'K': return DS_F | DS_E | DS_I | DS_K;
    case 'L': return DS_F | DS_E | DS_D;
    case 'M': return DS_F | DS_E | DS_B | DS_C | DS_H | DS_I;
    case 'N': return DS_F | DS_E | DS_B | DS_C | DS_H | DS_K;
    case 'O': return DS_A | DS_B | DS_C | DS_D | DS_E | DS_F;
    case 'P': return DS_A | DS_F | DS_B | DS_E | DS_G;
    case 'Q': return DS_A | DS_B | DS_C | DS_D | DS_E | DS_F | DS_K;
    case 'R': return DS_A | DS_F | DS_B | DS_E | DS_G | DS_K;
    case 'S': return DS_A | DS_F | DS_G | DS_C | DS_D;
    case 'T': return DS_A | DS_M | DS_N;
    case 'U': return DS_F | DS_E | DS_D | DS_B | DS_C;
    case 'V': return DS_F | DS_E | DS_J | DS_I;
    case 'W': return DS_F | DS_E | DS_B | DS_C | DS_J | DS_K;
    case 'X': return DS_H | DS_I | DS_J | DS_K;
    case 'Y': return DS_F | DS_B | DS_G | DS_N;
    case 'Z': return DS_A | DS_I | DS_J | DS_D;
    default: return 0;
  }
}

#define DSEG_LINE_BODY(g, x1, y1, x2, y2, col) do { \
  (g).drawLine((x1), (y1), (x2), (y2), (col)); \
  (g).drawLine((x1) + 1, (y1), (x2) + 1, (y2), (col)); \
  (g).drawLine((x1), (y1) + 1, (x2), (y2) + 1, (col)); \
} while (0)

static void dsegLine(m5gfx::M5GFX &g, int x1, int y1, int x2, int y2, uint16_t col) {
  DSEG_LINE_BODY(g, x1, y1, x2, y2, col);
}

static void dsegLine(M5Canvas &g, int x1, int y1, int x2, int y2, uint16_t col) {
  DSEG_LINE_BODY(g, x1, y1, x2, y2, col);
}

#define DSEG_DRAW_CHAR_BODY(g, x, y, c, col) do { \
  char _c = (c); \
  if (_c == ':') { \
    (g).fillRect((x) + 2, (y) + 4, 2, 2, (col)); \
    (g).fillRect((x) + 2, (y) + 10, 2, 2, (col)); \
  } else if (_c == '_') { \
    dsegLine((g), (x) + 1, (y) + 15, (x) + 7, (y) + 15, (col)); \
  } else if (_c == '+') { \
    dsegLine((g), (x) + 1, (y) + 8, (x) + 7, (y) + 8, (col)); \
    dsegLine((g), (x) + 4, (y) + 4, (x) + 4, (y) + 12, (col)); \
  } else if (_c == '%' || _c == '/') { \
    (g).fillRect((x) + 1, (y) + 2, 2, 2, (col)); \
    (g).fillRect((x) + 6, (y) + 12, 2, 2, (col)); \
    dsegLine((g), (x) + 7, (y) + 1, (x) + 1, (y) + 14, (col)); \
  } else { \
    uint16_t _m = dseg14Mask(_c); \
    if (_m & DS_A) dsegLine((g), (x) + 2, (y),     (x) + 7, (y),     (col)); \
    if (_m & DS_B) dsegLine((g), (x) + 8, (y) + 1, (x) + 8, (y) + 6, (col)); \
    if (_m & DS_C) dsegLine((g), (x) + 8, (y) + 9, (x) + 8, (y) + 14, (col)); \
    if (_m & DS_D) dsegLine((g), (x) + 2, (y) + 15, (x) + 7, (y) + 15, (col)); \
    if (_m & DS_E) dsegLine((g), (x),     (y) + 9, (x),     (y) + 14, (col)); \
    if (_m & DS_F) dsegLine((g), (x),     (y) + 1, (x),     (y) + 6, (col)); \
    if (_m & DS_G) dsegLine((g), (x) + 2, (y) + 8, (x) + 7, (y) + 8, (col)); \
    if (_m & DS_H) dsegLine((g), (x) + 1, (y) + 1, (x) + 3, (y) + 7, (col)); \
    if (_m & DS_I) dsegLine((g), (x) + 7, (y) + 1, (x) + 5, (y) + 7, (col)); \
    if (_m & DS_J) dsegLine((g), (x) + 3, (y) + 9, (x) + 1, (y) + 14, (col)); \
    if (_m & DS_K) dsegLine((g), (x) + 5, (y) + 9, (x) + 7, (y) + 14, (col)); \
    if (_m & DS_M) dsegLine((g), (x) + 4, (y) + 1, (x) + 4, (y) + 7, (col)); \
    if (_m & DS_N) dsegLine((g), (x) + 4, (y) + 9, (x) + 4, (y) + 14, (col)); \
  } \
} while (0)

static int dseg14CharWidth(char c) {
  if (c == ' ') return 5;
  if (c == ':') return 7;
  if (c == '_') return 11;
  if (c == '+') return 10;
  if (c == '%' || c == '/') return 8;
  return 12;
}

static int dseg14TextWidth(const char *text) {
  int w = 0;
  while (*text) w += dseg14CharWidth(*text++);
  return w;
}

static int drawDseg14Char(m5gfx::M5GFX &g, int x, int y, char c, uint16_t col) {
  DSEG_DRAW_CHAR_BODY(g, x, y, c, col);
  return dseg14CharWidth(c);
}

static int drawDseg14Char(M5Canvas &g, int x, int y, char c, uint16_t col) {
  DSEG_DRAW_CHAR_BODY(g, x, y, c, col);
  return dseg14CharWidth(c);
}

static void drawDseg14Text(m5gfx::M5GFX &g, int x, int y, const char *text, uint16_t col) {
  while (*text) x += drawDseg14Char(g, x, y, *text++, col);
}

static void drawDseg14Text(M5Canvas &g, int x, int y, const char *text, uint16_t col) {
  while (*text) x += drawDseg14Char(g, x, y, *text++, col);
}

static int filteredBatteryLevel() {
  uint32_t now = millis();
  if (g_batteryShown >= 0 && now - g_batteryLastSampleMs < 2000) return g_batteryShown;
  g_batteryLastSampleMs = now;
  int raw = M5.Power.getBatteryLevel();
  raw = max(0, min(100, raw));
  if (g_batteryShown < 0) {
    g_batteryShown = raw;
    g_batteryCandidate = raw;
    g_batteryCandidateHits = 0;
    return g_batteryShown;
  }
  int diff = raw - g_batteryShown;
  if (diff > 0) {
    g_batteryCandidate = raw;
    g_batteryCandidateHits = 0;
    g_batteryShown += min(diff, 3);
    return g_batteryShown;
  }
  if (abs(diff) <= 3) {
    g_batteryCandidate = raw;
    g_batteryCandidateHits = 0;
    if (diff < 0) g_batteryShown--;
    return g_batteryShown;
  }
  if (abs(diff) <= 12) {
    g_batteryCandidate = raw;
    g_batteryCandidateHits = 0;
    g_batteryShown += (diff > 0) ? 1 : -1;
    return g_batteryShown;
  }
  if (raw == g_batteryCandidate) {
    if (g_batteryCandidateHits < 255) g_batteryCandidateHits++;
  } else {
    g_batteryCandidate = raw;
    g_batteryCandidateHits = 1;
  }
  if (g_batteryCandidateHits >= 3) g_batteryShown += (diff > 0) ? 1 : -1;
  return g_batteryShown;
}

#define DRAW_STATUS_BATTERY(g) do { \
  int bat = filteredBatteryLevel(); \
  uint16_t col = (bat <= 20) ? COL_RED : COL_GREEN; \
  (g).fillRect(CONTENT_W - 62, 0, 62, 18, COL_BG); \
  (g).setTextColor(col, COL_BG); \
  char _bat[8]; snprintf(_bat, sizeof(_bat), "%d%%", bat); \
  drawDseg14Text((g), CONTENT_W - dseg14TextWidth(_bat) - 4, 1, _bat, col); \
} while (0)

static void drawStatusBattery(m5gfx::M5GFX &g) {
  DRAW_STATUS_BATTERY(g);
}

static void drawStatusBattery(M5Canvas &g) {
  DRAW_STATUS_BATTERY(g);
}

#define DRAW_STATUS_BATTERY_COLOR(g, okCol) do { \
  int bat = filteredBatteryLevel(); \
  uint16_t col = (bat <= 20) ? COL_RED : (okCol); \
  (g).fillRect(CONTENT_W - 62, 0, 62, 18, COL_BG); \
  (g).setTextColor(col, COL_BG); \
  char _bat[8]; snprintf(_bat, sizeof(_bat), "%d%%", bat); \
  drawDseg14Text((g), CONTENT_W - dseg14TextWidth(_bat) - 4, 1, _bat, col); \
} while (0)

static void drawStatusBatteryColor(m5gfx::M5GFX &g, uint16_t okCol) {
  DRAW_STATUS_BATTERY_COLOR(g, okCol);
}

static void drawStatusBatteryColor(M5Canvas &g, uint16_t okCol) {
  DRAW_STATUS_BATTERY_COLOR(g, okCol);
}

static void drawBattery() {
  drawStatusBattery(M5Cardputer.Display);
}

static void drawStatusBase(m5gfx::M5GFX &g) {
  g.fillRect(0, 0, CONTENT_W, 21, COL_BG);
  drawStatusBattery(g);
}

static void drawStatusBase(M5Canvas &g) {
  g.fillRect(0, 0, CONTENT_W, 21, COL_BG);
  drawStatusBattery(g);
}

#define DRAW_STATUS_TITLE_BODY(g, text, col) do { \
  drawStatusBase(g); \
  (g).setTextColor(col, COL_BG); \
  drawDseg14Text((g), 4, 1, (text), col); \
  (g).drawFastHLine(0, WAVE_TOP - 1, CONTENT_W, 0x0820); \
} while (0)

static void drawStatusTitle(m5gfx::M5GFX &g, const char *text, uint16_t col = COL_GREEN) {
  DRAW_STATUS_TITLE_BODY(g, text, col);
}

static void drawStatusTitle(M5Canvas &g, const char *text, uint16_t col = COL_GREEN) {
  DRAW_STATUS_TITLE_BODY(g, text, col);
}

static void drawStatusTitleNoLine(m5gfx::M5GFX &g, const char *text, uint16_t col = COL_GREEN) {
  g.fillRect(0, 0, CONTENT_W, 19, COL_BG);
  drawStatusBattery(g);
  g.setTextColor(col, COL_BG);
  drawDseg14Text(g, 4, 1, text, col);
}

static void drawStatusTitleNoLine(M5Canvas &g, const char *text, uint16_t col = COL_GREEN) {
  g.fillRect(0, 0, CONTENT_W, 19, COL_BG);
  drawStatusBattery(g);
  g.setTextColor(col, COL_BG);
  drawDseg14Text(g, 4, 1, text, col);
}

#define DRAW_STATUS_TAB_BODY(g, label, x, active) do { \
  uint16_t _tabCol = active ? COL_GREEN : COL_DIM; \
  (g).setTextColor(_tabCol, COL_BG); \
  drawDseg14Text((g), (x) + 4, 1, (label), _tabCol); \
} while (0)

static void drawStatusTab(m5gfx::M5GFX &g, const char *label, int x, bool active) {
  DRAW_STATUS_TAB_BODY(g, label, x, active);
}

static void drawStatusTab(M5Canvas &g, const char *label, int x, bool active) {
  DRAW_STATUS_TAB_BODY(g, label, x, active);
}

#define DRAW_STATUS_TABS_BODY(g, mode, count) do { \
  drawStatusBase(g); \
  drawStatusTab(g, "NOR", 4, (mode) == REC_NORMAL); \
  drawStatusTab(g, "KEY", 48, (mode) == REC_SHORTCUT); \
  drawStatusTab(g, "IMP", 92, (mode) == REC_IMPORTANT); \
  (g).setTextColor(COL_GREEN, COL_BG); \
  char _cnt[8]; snprintf(_cnt, sizeof(_cnt), "%d", count); \
  drawDseg14Text((g), 142, 1, _cnt, COL_GREEN); \
  (g).drawFastHLine(0, 20, CONTENT_W, COL_DIM); \
} while (0)

static void drawStatusTabs(m5gfx::M5GFX &g, uint8_t mode, int count) {
  DRAW_STATUS_TABS_BODY(g, mode, count);
}

static void drawStatusTabs(M5Canvas &g, uint8_t mode, int count) {
  DRAW_STATUS_TABS_BODY(g, mode, count);
}

// 底栏提示(小字, 上方一条分隔线)
static void drawFooter(const char *hint, uint16_t col = COL_DIM) {
  auto &d = M5Cardputer.Display;
  int y = d.height() - 15;
  d.drawFastHLine(0, y - 3, d.width(), col);
  d.fillRect(0, y, d.width(), 15, COL_BG);
  FONT_CN_12(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(6, y);
  d.print(hint);
}

static void drawActionToast(const char *msg, uint16_t col = COL_GREEN) {
  auto &d = M5Cardputer.Display;
  d.fillRect(74, 118, 92, 17, COL_BG);
  d.drawRect(74, 118, 92, 17, col);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  int x = 120 - (int)strlen(msg) * 3;
  if (x < 76) x = 76;
  d.setCursor(x, 121);
  d.print(msg);
}

static void drawCanvasToast(M5Canvas &cv, const char *msg, uint16_t col = COL_GREEN) {
  cv.fillRect(74, 58, 92, 20, COL_BG);
  cv.drawRect(74, 58, 92, 20, col);
  FONT_ASCII(cv);
  cv.setTextColor(col, COL_BG);
  int x = 120 - (int)strlen(msg) * 3;
  if (x < 76) x = 76;
  cv.setCursor(x, 62);
  cv.print(msg);
  cv.pushSprite(0, 0);
}

static void drawVolumeToast(uint16_t col = COL_GREEN) {
  auto &d = M5Cardputer.Display;
  const int x = 34, y = 116;
  const int barX = x + 42, barY = y + 3, blockW = 9, blockH = 11, gap = 3;
  int level = (playVol * VOL_LEVELS + 127) / 255;
  level = max(0, min(VOL_LEVELS, level));
  char levelBuf[6];
  snprintf(levelBuf, sizeof(levelBuf), "%02d/10", level);
  d.fillRect(0, 113, CONTENT_W, 22, COL_BG);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(x, y + 4);
  d.print("VOL");
  for (int i = 0; i < VOL_LEVELS; i++) {
    int bx = barX + i * (blockW + gap);
    d.fillRect(bx, barY, blockW, blockH, i < level ? col : 0x0440);
  }
  d.setCursor(194, y + 4);
  d.print(levelBuf);
}

static void drawCanvasVolumeToast(M5Canvas &cv, uint16_t col = COL_GREEN) {
  const int x = 34, y = 116;
  const int barX = x + 42, barY = y + 3, blockW = 9, blockH = 11, gap = 3;
  int level = (playVol * VOL_LEVELS + 127) / 255;
  level = max(0, min(VOL_LEVELS, level));
  char levelBuf[6];
  snprintf(levelBuf, sizeof(levelBuf), "%02d/10", level);
  cv.fillRect(0, 113, CONTENT_W, 22, COL_BG);
  FONT_ASCII(cv);
  cv.setTextColor(col, COL_BG);
  cv.setCursor(x, y + 4);
  cv.print("VOL");
  for (int i = 0; i < VOL_LEVELS; i++) {
    int bx = barX + i * (blockW + gap);
    cv.fillRect(bx, barY, blockW, blockH, i < level ? col : 0x0440);
  }
  cv.setCursor(194, y + 4);
  cv.print(levelBuf);
  cv.pushSprite(0, 0);
}

static void drawBrightnessToast(uint16_t col = COL_GREEN) {
  auto &d = M5Cardputer.Display;
  const int x = 52, y = 116;
  const int barX = x + 42, barY = y + 3, blockW = 18, blockH = 11, gap = 4;
  d.fillRect(0, 113, CONTENT_W, 22, COL_BG);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(x, y + 4);
  d.print("BRI");
  for (int i = 0; i < BRIGHT_LEVELS; i++) {
    int bx = barX + i * (blockW + gap);
    d.fillRect(bx, barY, blockW, blockH, i <= brightLevel ? col : 0x0440);
  }
}

static void drawCanvasBrightnessToast(M5Canvas &cv, uint16_t col = COL_GREEN) {
  const int x = 52, y = 116;
  const int barX = x + 42, barY = y + 3, blockW = 18, blockH = 11, gap = 4;
  cv.fillRect(0, 113, CONTENT_W, 22, COL_BG);
  FONT_ASCII(cv);
  cv.setTextColor(col, COL_BG);
  cv.setCursor(x, y + 4);
  cv.print("BRI");
  for (int i = 0; i < BRIGHT_LEVELS; i++) {
    int bx = barX + i * (blockW + gap);
    cv.fillRect(bx, barY, blockW, blockH, i <= brightLevel ? col : 0x0440);
  }
  cv.pushSprite(0, 0);
}

static void drawCanvasSaveBadge(M5Canvas &cv) {
  const int x = CONTENT_W - 78;
  const int y = 114;
  cv.fillRect(x - 4, y - 3, 82, 24, COL_BG);
  drawDseg14Text(cv, x, y, "SAVED", COL_GREEN);
  cv.fillRect(x, y + 17, 54, 3, COL_GREEN);
  cv.pushSprite(0, 0);
}

static void showMsg(const char *title, const char *msg, uint16_t col) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader(title);
  FONT_CN_16(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(12, 56);
  d.print(msg);
  drawFooter("按任意键返回");
  waitRelease();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    delay(8);
  }
}

// ---------- 快捷键绑定: 读/写 SD ----------
static void saveHotkeys();

static void loadHotkeys() {
  hotkeyCount = 0;
  if (!sdMount()) return;
  bool migrated = false;
  File f = SD.open(HOTKEY_PATH, FILE_READ);
  if (!f) {
    f = SD.open(OLD_IMPORTANT_HOTKEY_PATH, FILE_READ);
    migrated = (bool)f;
  }
  if (!f) {
    f = SD.open(OLD_HOTKEY_PATH, FILE_READ);
    migrated = (bool)f;
  }
  if (f) {
    while (f.available() && hotkeyCount < MAX_HOTKEY) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 3) continue;        // 格式: "<键> <编号>"
      char k = normalizeBindKey(line.charAt(0));
      int idx = line.substring(2).toInt();
      if (k && idx > 0) { hotkeys[hotkeyCount].key = k; hotkeys[hotkeyCount].idx = idx; hotkeyCount++; }
    }
    f.close();
  }
  SD.end();
  if (migrated) saveHotkeys();
}

static void ensureHotkeysLoaded() {
  if (hotkeysLoaded) return;
  loadHotkeys();
  hotkeysLoaded = true;
}

static void saveHotkeys() {
  if (!sdMount()) return;
  if (!SD.exists(SHORTCUT_DIR)) SD.mkdir(SHORTCUT_DIR);
  const char *tmp = "/SHORTCUT/keys.tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (f) {
    for (int i = 0; i < hotkeyCount; i++) f.printf("%c %d\n", hotkeys[i].key, hotkeys[i].idx);
    f.flush();
    f.close();
    SD.remove(HOTKEY_PATH);
    if (!SD.rename(tmp, HOTKEY_PATH)) SD.remove(tmp);
  }
  SD.end();
}

static int findHotkey(char k) {
  k = normalizeBindKey(k);
  if (!k) return -1;
  for (int i = 0; i < hotkeyCount; i++) if (hotkeys[i].key == k) return hotkeys[i].idx;
  return -1;
}

// 给某录音编号绑定一个键(同键覆盖, 同编号原有的键先清掉, 保证一键一录音)
static void setHotkey(char k, int idx) {
  k = normalizeBindKey(k);
  if (!k || idx <= 0) return;
  for (int i = 0; i < hotkeyCount; i++) {
    if (hotkeys[i].key == k || hotkeys[i].idx == idx) {
      for (int j = i; j < hotkeyCount - 1; j++) hotkeys[j] = hotkeys[j + 1];
      hotkeyCount--; i--;
    }
  }
  if (hotkeyCount < MAX_HOTKEY) { hotkeys[hotkeyCount].key = k; hotkeys[hotkeyCount].idx = idx; hotkeyCount++; }
  saveHotkeys();
}

static char hotkeyOf(int idx) {  // 该录音编号是否已绑定某键, 返回键(0=无)
  for (int i = 0; i < hotkeyCount; i++) if (hotkeys[i].idx == idx) return hotkeys[i].key;
  return 0;
}

// 当前是否按下了一个"已绑定的播放键"(非 Ctrl/Tab): 返回对应录音编号, 否则 -1
static int pressedHotkeyRec() {
  if (keyCtrl() || keyTab()) return -1;
  if (keyEnter() || keyDel() || keySpace() || keyUpload()) return -1;
  char bk = pressedBindKey();
  if (!bk) return -1;
  return findHotkey(bk);             // >0 已绑定; -1 未绑定
}

// ---------- 列出录音编号 ----------
int recList[MAX_REC];
int recCount = 0;
static uint8_t shortcutBits[(MAX_REC + 8) / 8];
static uint8_t importantBits[(MAX_REC + 8) / 8];
static uint8_t frictionDoneBits[(MAX_REC + 8) / 8];
static uint8_t frictionPendingBits[(MAX_REC + 8) / 8];
static uint8_t uploadQueuedBits[(MAX_REC + 8) / 8];
static uint8_t uploadDoneBits[(MAX_REC + 8) / 8];
static uint8_t uploadPendingBits[(MAX_REC + 8) / 8];
static uint8_t uploadModelErrBits[(MAX_REC + 8) / 8];
static uint8_t uploadJobErrBits[(MAX_REC + 8) / 8];
static uint16_t recDurationSec[MAX_REC];

static void clearImportantBits() {
  memset(shortcutBits, 0, sizeof(shortcutBits));
  memset(importantBits, 0, sizeof(importantBits));
  memset(recDurationSec, 0, sizeof(recDurationSec));
}

static bool bitIsSet(uint8_t *bits, int recNum) {
  if (recNum <= 0 || recNum > MAX_REC) return false;
  int bit = recNum - 1;
  return (bits[bit >> 3] & (1 << (bit & 7))) != 0;
}

static void setBit(uint8_t *bits, int recNum, bool on) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (on) bits[bit >> 3] |= (1 << (bit & 7));
  else bits[bit >> 3] &= ~(1 << (bit & 7));
}

static bool frictionDone(int recNum) { return bitIsSet(frictionDoneBits, recNum); }
static bool frictionPending(int recNum) { return bitIsSet(frictionPendingBits, recNum); }
static void setFrictionDone(int recNum, bool on) { setBit(frictionDoneBits, recNum, on); }
static void setFrictionPending(int recNum, bool on) { setBit(frictionPendingBits, recNum, on); }
static bool uploadQueued(int recNum) { return bitIsSet(uploadQueuedBits, recNum); }
static bool uploadDone(int recNum) { return bitIsSet(uploadDoneBits, recNum); }
static bool uploadPending(int recNum) { return bitIsSet(uploadPendingBits, recNum); }
static bool uploadModelErr(int recNum) { return bitIsSet(uploadModelErrBits, recNum); }
static bool uploadJobErr(int recNum) { return bitIsSet(uploadJobErrBits, recNum); }
static void setUploadQueued(int recNum, bool on) { setBit(uploadQueuedBits, recNum, on); }
static void setUploadDone(int recNum, bool on) { setBit(uploadDoneBits, recNum, on); }
static void setUploadPending(int recNum, bool on) { setBit(uploadPendingBits, recNum, on); }
static void setUploadModelErr(int recNum, bool on) { setBit(uploadModelErrBits, recNum, on); }
static void setUploadJobErr(int recNum, bool on) { setBit(uploadJobErrBits, recNum, on); }

static void loadUploadBitFile(const char *path, uint8_t *bits) {
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  char line[32];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    int recNum = atoi(line);
    if (recNum > 0) setBit(bits, recNum, true);
  }
  f.close();
}

static void loadUploadStateMounted() {
  memset(uploadQueuedBits, 0, sizeof(uploadQueuedBits));
  memset(uploadDoneBits, 0, sizeof(uploadDoneBits));
  memset(uploadPendingBits, 0, sizeof(uploadPendingBits));
  memset(uploadModelErrBits, 0, sizeof(uploadModelErrBits));
  memset(uploadJobErrBits, 0, sizeof(uploadJobErrBits));
  loadUploadBitFile(UPLOAD_QUEUE_PATH, uploadQueuedBits);
  loadUploadBitFile(UPLOAD_DONE_PATH, uploadDoneBits);
  loadUploadBitFile(UPLOAD_PENDING_PATH, uploadPendingBits);
  loadUploadBitFile(UPLOAD_MODEL_ERR_PATH, uploadModelErrBits);
  loadUploadBitFile(UPLOAD_JOB_ERR_PATH, uploadJobErrBits);
}

static void loadRecVolumeStateMounted() {
  memset(recVolumeDelta, 0, sizeof(recVolumeDelta));
  recVolumeDirty = false;
  File f = SD.open(REC_VOLUME_PATH, FILE_READ);
  if (!f) return;
  char line[32];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    int recNum = 0;
    int delta = 0;
    if (sscanf(line, "%d %d", &recNum, &delta) == 2 && recNum > 0 && recNum <= MAX_REC) {
      recVolumeDelta[recNum - 1] = (int8_t)max(-VOL_LEVELS, min(VOL_LEVELS, delta));
    }
  }
  f.close();
}

static bool saveRecVolumeStateMounted() {
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  const char *tmp = "/REC/volume.tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  for (int i = 0; i < MAX_REC; i++) {
    int8_t delta = recVolumeDelta[i];
    if (delta != 0) f.printf("%d %d\n", i + 1, (int)delta);
  }
  f.close();
  SD.remove(REC_VOLUME_PATH);
  bool ok = SD.rename(tmp, REC_VOLUME_PATH);
  if (!ok) SD.remove(tmp);
  if (ok) recVolumeDirty = false;
  return ok;
}

static int adjustRecVolumeDelta(int recNum, int delta) {
  if (recNum <= 0 || recNum > MAX_REC) return 0;
  int next = recVolumeDelta[recNum - 1] + (delta > 0 ? 1 : -1);
  next = max(-VOL_LEVELS, min(VOL_LEVELS, next));
  recVolumeDelta[recNum - 1] = (int8_t)next;
  recVolumeDirty = true;
  return next;
}

static uint16_t durationFromFileSize(uint32_t fileSize) {
  if (fileSize <= 44) return 0;
  uint32_t sec = (((fileSize - 44) / 2) + REC_RATE - 1) / REC_RATE;
  return (sec > 65535) ? 65535 : (uint16_t)sec;
}

static uint32_t playableWavDataBytes(File &f) {
  uint32_t total = f.size();
  if (total <= 44) return 0;
  uint8_t h[12];
  if (!f.seek(0) || f.read(h, sizeof(h)) != (int)sizeof(h)) return 0;
  if (h[0] != 'R' || h[1] != 'I' || h[2] != 'F' || h[3] != 'F') return 0;
  if (h[8] != 'W' || h[9] != 'A' || h[10] != 'V' || h[11] != 'E') return 0;
  uint8_t dataTag[4];
  if (!f.seek(36) || f.read(dataTag, sizeof(dataTag)) != (int)sizeof(dataTag)) return 0;
  if (dataTag[0] != 'd' || dataTag[1] != 'a' || dataTag[2] != 't' || dataTag[3] != 'a') return 0;
  uint8_t hb[4];
  if (f.read(hb, sizeof(hb)) != (int)sizeof(hb)) return 0;
  uint32_t dataSize = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  uint32_t fileData = total - 44;
  if (dataSize == 0 || dataSize > fileData) return 0;
  return dataSize & ~1UL;
}

static uint32_t wavSampleRate(File &f) {
  uint8_t hb[4];
  if (!f.seek(24) || f.read(hb, sizeof(hb)) != (int)sizeof(hb)) return REC_RATE;
  uint32_t rate = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  if (rate < 8000 || rate > 48000) return REC_RATE;
  return rate;
}

static void setRecDuration(int recNum, uint32_t fileSize) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  recDurationSec[recNum - 1] = durationFromFileSize(fileSize);
}

static uint16_t getRecDuration(int recNum) {
  if (recNum <= 0 || recNum > MAX_REC) return 0;
  return recDurationSec[recNum - 1];
}

static void formatDuration(uint16_t sec, char *buf, size_t n) {
  if (sec >= 3600) snprintf(buf, n, "%uh%02u", (unsigned)(sec / 3600), (unsigned)((sec / 60) % 60));
  else snprintf(buf, n, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

static bool isShortcutRec(int recNum) {
  if (recNum <= 0 || recNum > MAX_REC) return false;
  int bit = recNum - 1;
  return (shortcutBits[bit >> 3] & (1 << (bit & 7))) != 0;
}

static bool isImportantRec(int recNum) {
  if (recNum <= 0 || recNum > MAX_REC) return false;
  int bit = recNum - 1;
  return (importantBits[bit >> 3] & (1 << (bit & 7))) != 0;
}

static void setShortcutRec(int recNum, bool shortcut) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (shortcut) shortcutBits[bit >> 3] |= (1 << (bit & 7));
  else shortcutBits[bit >> 3] &= ~(1 << (bit & 7));
}

static void setImportantRec(int recNum, bool important) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (important) importantBits[bit >> 3] |= (1 << (bit & 7));
  else importantBits[bit >> 3] &= ~(1 << (bit & 7));
}

static uint8_t recKindOf(int recNum) {
  if (isShortcutRec(recNum)) return REC_SHORTCUT;
  if (isImportantRec(recNum)) return REC_IMPORTANT;
  return REC_NORMAL;
}

static void recordingPathForRec(int recNum, char *p, size_t n) {
  recordingPathKind(recNum, recKindOf(recNum), p, n);
}

static int recListIndexOf(int recNum) {
  for (int i = 0; i < recCount; i++) if (recList[i] == recNum) return i;
  return -1;
}

static void insertRecListSorted(int recNum, uint8_t kind = REC_NORMAL) {
  if (recNum <= 0) return;
  int existing = recListIndexOf(recNum);
  if (existing >= 0) {
    if (kind == REC_SHORTCUT) setShortcutRec(recNum, true);
    if (kind == REC_IMPORTANT) setImportantRec(recNum, true);
    return;
  }
  if (recCount >= MAX_REC) return;
  int pos = recCount;
  while (pos > 0 && recList[pos - 1] > recNum) {
    recList[pos] = recList[pos - 1];
    pos--;
  }
  recList[pos] = recNum;
  setShortcutRec(recNum, kind == REC_SHORTCUT);
  setImportantRec(recNum, kind == REC_IMPORTANT);
  recCount++;
}

static void insertRecListAtEnd(int recNum, uint8_t kind = REC_NORMAL) {
  if (recNum <= 0) return;
  int existing = recListIndexOf(recNum);
  if (existing >= 0) {
    if (kind == REC_SHORTCUT) setShortcutRec(recNum, true);
    if (kind == REC_IMPORTANT) setImportantRec(recNum, true);
    return;
  }
  if (recCount >= MAX_REC) return;
  recList[recCount++] = recNum;
  setShortcutRec(recNum, kind == REC_SHORTCUT);
  setImportantRec(recNum, kind == REC_IMPORTANT);
}

static void applyRecordingOrderMounted() {
  File f = SD.open(REC_ORDER_PATH, FILE_READ);
  if (!f) return;
  static uint8_t orderedBits[(MAX_REC + 8) / 8];
  memset(orderedBits, 0, sizeof(orderedBits));
  bool hasOrdered = false;
  char line[24];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    int recNum = atoi(line);
    if (recNum <= 0) continue;
    if (bitIsSet(orderedBits, recNum)) continue;
    if (recListIndexOf(recNum) < 0) continue;
    setBit(orderedBits, recNum, true);
    hasOrdered = true;
  }
  f.close();
  if (!hasOrdered) return;
  int write = 0;
  for (int i = 0; i < recCount; i++) {
    if (!bitIsSet(orderedBits, recList[i])) recList[write++] = recList[i];
  }
  f = SD.open(REC_ORDER_PATH, FILE_READ);
  if (!f) return;
  while (f.available() && write < recCount) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    int recNum = atoi(line);
    if (!bitIsSet(orderedBits, recNum)) continue;
    recList[write++] = recNum;
    setBit(orderedBits, recNum, false);
  }
  f.close();
}

static void removeRecordingOrderMounted(int recNum) {
  if (recNum <= 0) return;
  File in = SD.open(REC_ORDER_PATH, FILE_READ);
  if (!in) return;
  const char *tmp = "/REC/.order.tmp";
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  char line[24];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (atoi(line) == recNum) continue;
    if (out && line[0]) out.printf("%s\n", line);
  }
  in.close();
  if (out) {
    out.close();
    SD.remove(REC_ORDER_PATH);
    if (!SD.rename(tmp, REC_ORDER_PATH)) SD.remove(tmp);
  } else {
    SD.remove(tmp);
  }
}

static void appendRecordingOrderMounted(int recNum) {
  if (recNum <= 0) return;
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  removeRecordingOrderMounted(recNum);
  File f = SD.open(REC_ORDER_PATH, FILE_APPEND);
  if (!f) return;
  f.printf("%d\n", recNum);
  f.close();
}

static void removeRecListAt(int idx) {
  if (idx < 0 || idx >= recCount) return;
  for (int i = idx; i < recCount - 1; i++) recList[i] = recList[i + 1];
  recCount--;
}

static bool recordingExistsKind(int recNum, uint8_t kind) {
  char p[40];
  recordingPathKind(recNum, kind, p, sizeof(p));
  return SD.exists(p);
}

static int readNextIndexCache() {
  File f = SD.open(NEXT_INDEX_PATH, FILE_READ);
  if (!f) return 0;
  int idx = f.parseInt();
  f.close();
  return (idx >= 1 && idx <= 9999) ? idx : 0;
}

static void writeNextIndexCache(int idx) {
  if (idx < 1) idx = 1;
  if (idx > 9999) idx = 9999;
  SD.remove(NEXT_INDEX_PATH);
  File f = SD.open(NEXT_INDEX_PATH, FILE_WRITE);
  if (!f) return;
  f.printf("%d\n", idx);
  f.flush();
  f.close();
}

static void scanRecordingDir(const char *dirPath, uint8_t kind, int &maxIdx) {
  File dir = SD.open(dirPath);
  if (dir && dir.isDirectory()) {
    File e;
    while ((e = dir.openNextFile())) {
      int n = parseRecordingNumber(e.name());
      uint32_t dataBytes = (!e.isDirectory()) ? playableWavDataBytes(e) : 0;
      if (n > 0 && dataBytes > 0) {
        if (n > maxIdx) maxIdx = n;
        insertRecListSorted(n, kind);
        setRecDuration(n, 44 + dataBytes);
      }
      e.close();
    }
    dir.close();
  }
}

static int lowestAvailableRecordingIndex() {
  char p[40];
  for (int idx = 1; idx < 9999; idx++) {
    recordingPathKind(idx, REC_NORMAL, p, sizeof(p));
    if (!SD.exists(p) && !recordingExistsKind(idx, REC_SHORTCUT) && !recordingExistsKind(idx, REC_IMPORTANT)) return idx;
  }
  return 9999;
}

static int nextRecordingIndex() {
  int idx = nextRecHint;
  if (idx < 1 || idx > 9999) idx = readNextIndexCache();
  if (idx < 1 || idx > 9999) idx = 1;
  int start = idx;
  do {
    if (!recordingExistsKind(idx, REC_NORMAL) &&
        !recordingExistsKind(idx, REC_SHORTCUT) &&
        !recordingExistsKind(idx, REC_IMPORTANT)) {
      nextRecHint = idx;
      return idx;
    }
    idx = (idx < 9999) ? idx + 1 : 1;
  } while (idx != start);
  idx = lowestAvailableRecordingIndex();
  nextRecHint = idx;
  writeNextIndexCache(idx);
  return idx;
}

static void scanRecordings(bool compactNext = false) {
  recCount = 0;
  clearImportantBits();
  if (!sdMount()) return;
  if (!SD.exists(REC_DIR) && !SD.exists(SHORTCUT_DIR) && !SD.exists(IMPORTANT_DIR)) { SD.end(); return; }
  int maxIdx = 0;
  scanRecordingDir(REC_DIR, REC_NORMAL, maxIdx);
  scanRecordingDir(SHORTCUT_DIR, REC_SHORTCUT, maxIdx);
  scanRecordingDir(IMPORTANT_DIR, REC_IMPORTANT, maxIdx);
  applyRecordingOrderMounted();
  loadUploadStateMounted();
  loadRecVolumeStateMounted();
  nextRecHint = lowestAvailableRecordingIndex();
  writeNextIndexCache(nextRecHint);
  SD.end();
  bool hotkeyChanged = false;
  for (int i = 0; i < hotkeyCount; i++) {
    if (!isShortcutRec(hotkeys[i].idx)) {
      for (int j = i; j < hotkeyCount - 1; j++) hotkeys[j] = hotkeys[j + 1];
      hotkeyCount--;
      i--;
      hotkeyChanged = true;
    }
  }
  if (hotkeyChanged) saveHotkeys();
}

// ---------- 提示音 "滴" (淡入淡出) ----------
// (保留: 暂未使用; 需要时可在切到扬声器后调用以遮爆音)

// ---------- 切换编解码器到扬声器(含手动开 DAC) ----------
static bool copyFileOnSD(const char *from, const char *to) {
  File in = SD.open(from, FILE_READ);
  if (!in) return false;
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.t", to);
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { in.close(); return false; }
  uint8_t buf[512];
  while (in.available()) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (out.write(buf, n) != (size_t)n) {
      in.close();
      out.close();
      SD.remove(tmp);
      return false;
    }
  }
  out.flush();
  in.close();
  out.close();
  SD.remove(to);
  if (SD.rename(tmp, to)) return true;
  SD.remove(tmp);
  return false;
}

static bool moveRecordingToKind(int recNum, uint8_t targetKind) {
  if (targetKind == REC_NORMAL) return false;
  if ((targetKind == REC_SHORTCUT && isShortcutRec(recNum)) ||
      (targetKind == REC_IMPORTANT && isImportantRec(recNum))) return true;
  if (!sdMount()) return false;
  const char *targetDir = (targetKind == REC_SHORTCUT) ? SHORTCUT_DIR : IMPORTANT_DIR;
  if (!SD.exists(targetDir)) SD.mkdir(targetDir);
  char src[40], dst[40];
  recordingPathKind(recNum, targetKind, dst, sizeof(dst));
  uint8_t srcKind = recKindOf(recNum);
  if (srcKind == targetKind) srcKind = REC_NORMAL;
  recordingPathKind(recNum, srcKind, src, sizeof(src));
  bool ok = false;
  if (SD.exists(src)) {
    if (srcKind == REC_NORMAL) ok = SD.rename(src, dst);
    else ok = copyFileOnSD(src, dst);
  }
  if (!ok && SD.exists(src)) {
    ok = copyFileOnSD(src, dst);
    if (ok && srcKind == REC_NORMAL) SD.remove(src);
  }
  if (!ok && !SD.exists(src) && SD.exists(dst)) ok = true;
  SD.end();
  if (ok) {
    if (targetKind == REC_SHORTCUT) setShortcutRec(recNum, true);
    if (targetKind == REC_IMPORTANT) setImportantRec(recNum, true);
  }
  return ok;
}

static bool markRecordingImportant(int recNum) {
  return moveRecordingToKind(recNum, REC_IMPORTANT);
}

static bool replaceFileWithTemp(const char *path, const char *tmp) {
  char bak[48];
  snprintf(bak, sizeof(bak), "%s.b", path);
  SD.remove(bak);
  if (!SD.exists(path) || !SD.rename(path, bak)) {
    SD.remove(tmp);
    return false;
  }
  if (SD.rename(tmp, path)) {
    SD.remove(bak);
    return true;
  }
  SD.rename(bak, path);
  SD.remove(tmp);
  return false;
}

static bool trimSilenceInWav(const char *path, int32_t threshold, uint32_t headPad, uint32_t tailPad) {
  File in = SD.open(path, FILE_READ);
  if (!in || in.size() <= 44 + REC_N * 2) { if (in) in.close(); return false; }

  uint32_t dataBytes = in.size() - 44;
  uint32_t totalSamples = dataBytes / 2;
  uint32_t firstActive = UINT32_MAX;
  uint32_t lastActive = 0;
  uint32_t sampleBase = 0;
  uint32_t remaining = dataBytes;
  static int16_t trimBuf[REC_N];

  in.seek(44);
  while (remaining > 0) {
    size_t want = remaining < sizeof(trimBuf) ? remaining : sizeof(trimBuf);
    int rd = in.read((uint8_t *)trimBuf, want);
    if (rd <= 0) break;
    int got = rd / 2;
    int64_t sumSq = 0;
    for (int i = 0; i < got; i++) sumSq += (int64_t)trimBuf[i] * trimBuf[i];
    int32_t rms = (int32_t)sqrtf((float)((double)sumSq / got));
    if (rms >= threshold) {
      if (firstActive == UINT32_MAX) firstActive = sampleBase;
      lastActive = sampleBase + got;
    }
    uint32_t used = got * 2;
    remaining = (remaining > used) ? (remaining - used) : 0;
    sampleBase += got;
  }

  if (firstActive == UINT32_MAX || lastActive <= firstActive) { in.close(); return false; }
  uint32_t startSample = (firstActive > headPad) ? (firstActive - headPad) : 0;
  uint32_t endSample = lastActive + tailPad;
  if (endSample > totalSamples) endSample = totalSamples;
  if (startSample == 0 && endSample >= totalSamples) { in.close(); return false; }

  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.t", path);
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { in.close(); return false; }

  writeWavHeader(out, REC_RATE, 0);
  uint32_t outSamples = endSample - startSample;
  uint32_t outIndex = 0;
  remaining = outSamples * 2;
  in.seek(44 + startSample * 2);
  while (remaining > 0) {
    size_t want = remaining < sizeof(trimBuf) ? remaining : sizeof(trimBuf);
    int rd = in.read((uint8_t *)trimBuf, want);
    if (rd <= 0) break;
    int got = rd / 2;
    for (int i = 0; i < got; i++, outIndex++) {
      int32_t gain = 256;
      if (outIndex < SHORTCUT_FADE_SAMPLES) gain = (int32_t)(outIndex * 256 / SHORTCUT_FADE_SAMPLES);
      uint32_t tailLeft = outSamples - outIndex;
      if (tailLeft < SHORTCUT_FADE_SAMPLES) {
        int32_t tailGain = (int32_t)(tailLeft * 256 / SHORTCUT_FADE_SAMPLES);
        if (tailGain < gain) gain = tailGain;
      }
      trimBuf[i] = (int16_t)(((int32_t)trimBuf[i] * gain) >> 8);
    }
    out.write((uint8_t *)trimBuf, got * 2);
    uint32_t used = got * 2;
    remaining = (remaining > used) ? (remaining - used) : 0;
  }

  uint32_t outBytes = outIndex * 2;
  writeWavHeader(out, REC_RATE, outBytes);
  out.flush();
  out.close();
  in.close();
  if (outBytes == 0) { SD.remove(tmp); return false; }
  return replaceFileWithTemp(path, tmp);
}

static bool trimShortcutSilenceForRec(int recNum, bool strong = false) {
  if (recNum <= 0) return false;
  if (!sdMount()) return false;
  char p[40];
  recordingPathKind(recNum, REC_SHORTCUT, p, sizeof(p));
  bool ok = strong
    ? trimSilenceInWav(p, SHORTCUT_RETRIM_RMS, SHORTCUT_RETRIM_HEAD_PAD, SHORTCUT_RETRIM_TAIL_PAD)
    : trimSilenceInWav(p, SHORTCUT_TRIM_RMS, SHORTCUT_HEAD_PAD, SHORTCUT_TAIL_PAD);
  if (ok) {
    File f = SD.open(p, FILE_READ);
    if (f) { setRecDuration(recNum, f.size()); f.close(); }
  }
  SD.end();
  return ok;
}

static bool markRecordingShortcut(int recNum) {
  bool alreadyShortcut = isShortcutRec(recNum);
  bool ok = moveRecordingToKind(recNum, REC_SHORTCUT);
  if (ok) trimShortcutSilenceForRec(recNum, alreadyShortcut);
  return ok;
}

static void speakerOn() {
  if (speakerOutputReady) {
    M5Cardputer.Speaker.setVolume(playVol);
    return;
  }
  micInputReady = false;
  M5Cardputer.Mic.end();       // 关麦(释放共用编解码器)
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(playVol);
  const uint8_t ES = 0x18;     // internal_spk=false 无自动 DAC 回调, 手动开(照官方扬声器寄存器)
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x01, 0xB5, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x02, 0x18, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x12, 0x00, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x10, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0xBF, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x37, 0x08, 400000);
  speakerOutputReady = true;
}

static void playSeekFeedback(bool forward) {
  const size_t n = sizeof(seekToneBuf) / sizeof(seekToneBuf[0]);
  for (size_t i = 0; i < n; i++) {
    int span = (int)(i * 20 / n);
    int period = forward ? (34 - span) : (14 + span);
    if (period < 8) period = 8;
    int amp = forward ? 1700 : 1350;
    int saw = ((int)(i % period) * 2 * amp / period) - amp;
    int edge = (i < n - i) ? i : (n - i);
    int env = edge < 48 ? edge : 48;
    seekToneBuf[i] = (int16_t)(saw * env / 48);
  }
  M5Cardputer.Speaker.playRaw(seekToneBuf, n, forward ? 24000 : 12000, false, 1, 0, false);
}

static const int WAVE_BARS = 60;   // 60 × 4px = 240px (CONTENT_W)
static int8_t waveBars[WAVE_BARS]; // A线采样点: -A_HALF..+A_HALF
static uint8_t waveBarCounts[WAVE_BARS];
static int8_t recLiveWave[CONTENT_W];      // B线视觉缓存: 录音监听线
static int8_t playbackLiveWave[CONTENT_W]; // B线视觉缓存: 播放监听线

static void mergePlaybackWaveBar(int idx, int amp) {
  if (idx < 0 || idx >= WAVE_BARS) return;
  if (amp > A_HALF) amp = A_HALF;
  if (amp < -A_HALF) amp = -A_HALF;
  if (waveBarCounts[idx] == 0 || abs(amp) > abs((int)waveBars[idx])) waveBars[idx] = (int8_t)amp;
  if (waveBarCounts[idx] < 255) waveBarCounts[idx]++;
}

static void feedPlaybackWaveBars(uint32_t chunkStart, uint32_t dataSize, const int16_t *buf, size_t n) {
  if (!buf || n == 0 || dataSize == 0) return;
  int amp = calcTrackAmp(buf, n);

  uint32_t chunkEnd = chunkStart + (uint32_t)n * 2;
  if (chunkEnd > dataSize) chunkEnd = dataSize;
  int first = (int)((uint64_t)chunkStart * WAVE_BARS / dataSize);
  int last  = (int)((uint64_t)(chunkEnd ? chunkEnd - 1 : chunkStart) * WAVE_BARS / dataSize);
  if (first < 0) first = 0;
  if (last >= WAVE_BARS) last = WAVE_BARS - 1;
  for (int i = first; i <= last; i++) mergePlaybackWaveBar(i, amp);
}

static bool previewPlaybackWaveBar(File &f, uint32_t dataSize, uint8_t &previewBar) {
  if (previewBar >= WAVE_BARS || dataSize == 0) return false;
  static int16_t tmp[REC_N];
  uint32_t restore = f.position();
  uint32_t segStart = (uint32_t)((uint64_t)dataSize * previewBar / WAVE_BARS);
  uint32_t segEnd = (uint32_t)((uint64_t)dataSize * (previewBar + 1) / WAVE_BARS);
  if (segEnd <= segStart) segEnd = segStart + 2;
  if (segEnd > dataSize) segEnd = dataSize;
  uint32_t span = segEnd - segStart;
  uint32_t pos = segStart + span / 2;
  if (pos + sizeof(tmp) > segEnd) pos = (segEnd > sizeof(tmp)) ? (segEnd - sizeof(tmp)) : segStart;
  pos &= ~1U;
  uint32_t want = dataSize - pos;
  if (want > sizeof(tmp)) want = sizeof(tmp);
  if (want > segEnd - pos) want = segEnd - pos;
  want &= ~1U;
  int rd = 0;
  if (want > 0 && f.seek(44 + pos)) rd = f.read((uint8_t *)tmp, want);
  if (rd > 0) feedPlaybackWaveBars(pos, dataSize, tmp, rd / 2);
  f.seek(restore);
  previewBar++;
  return true;
}

static void previewPlaybackWaveStep(File &f, uint32_t dataSize, uint8_t &previewBar, uint8_t budget) {
  while (budget-- > 0 && previewPlaybackWaveBar(f, dataSize, previewBar)) {}
}

static int deleteProgressW(uint32_t heldMs) {
  if (heldMs > DELETE_HOLD_MS) heldMs = DELETE_HOLD_MS;
  return (int)((uint32_t)70 * heldMs / DELETE_HOLD_MS);
}

static void drawDeleteProgress(M5Canvas &cv, uint32_t heldMs) {
  if (heldMs == 0) return;
  cv.fillRect(112, 119, 126, 15, COL_BG);
  FONT_CN_12(cv);
  cv.setTextColor(COL_RED, COL_BG);
  cv.setCursor(112, 120);
  cv.print("删除中");
  cv.drawRect(168, 125, 70, 7, COL_RED);
  cv.fillRect(168, 125, deleteProgressW(heldMs), 7, COL_RED);
}

static void drawDeleteProgress(m5gfx::M5GFX &g, uint32_t heldMs) {
  if (heldMs == 0) return;
  g.fillRect(112, 119, 126, 15, COL_BG);
  FONT_CN_12(g);
  g.setTextColor(COL_RED, COL_BG);
  g.setCursor(112, 120);
  g.print("删除中");
  g.drawRect(168, 125, 70, 7, COL_RED);
  g.fillRect(168, 125, deleteProgressW(heldMs), 7, COL_RED);
}

static void drawDeleteProgressOnDisplay(uint32_t heldMs) {
  auto &d = M5Cardputer.Display;
  d.fillRect(112, 119, 126, 15, COL_BG);
  if (heldMs == 0) return;
  FONT_CN_12(d);
  d.setTextColor(COL_RED, COL_BG);
  d.setCursor(112, 120);
  d.print("删除中");
  d.drawRect(168, 125, 70, 7, COL_RED);
  d.fillRect(168, 125, deleteProgressW(heldMs), 7, COL_RED);
}

static void drawDeleteProgressLocal(M5Canvas &cv, uint32_t heldMs) {
  if (heldMs == 0) return;
  int y = 119 - CHROME_TOP;
  int barY = 125 - CHROME_TOP;
  FONT_ASCII(cv);
  cv.setTextColor(COL_RED, COL_BG);
  cv.setCursor(112, y + 1);
  cv.print("DEL");
  cv.drawRect(168, barY, 70, 7, COL_RED);
  cv.fillRect(168, barY, deleteProgressW(heldMs), 7, COL_RED);
}

static void drawRecBottomCanvas(M5Canvas &cv, uint32_t elapsedMs, uint32_t deleteHeldMs = 0) {
  cv.fillScreen(COL_BG);
  uint32_t s = elapsedMs / 1000;
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(cv, 4, 1, recTime, COL_GREEN);
  drawDeleteProgressLocal(cv, deleteHeldMs);
}

static void drawPlaybackBottomCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, uint32_t deleteHeldMs = 0) {
  cv.fillScreen(COL_BG);
  uint32_t cur = (played / 2) / REC_RATE;
  uint32_t tot = (dataSize / 2) / REC_RATE;
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(cv, 4, 1, curBuf, COL_GREEN);
  if (deleteHeldMs > 0) {
    drawDeleteProgressLocal(cv, deleteHeldMs);
  } else {
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(cv, CONTENT_W - dseg14TextWidth(totBuf) - 4, 1, totBuf, COL_DIM);
  }
}

static void drawTrackBar(m5gfx::M5GFX &g, int i, int v) {
  int x0 = i * CONTENT_W / WAVE_BARS;
  int x1 = (i + 1) * CONTENT_W / WAVE_BARS;
  int x = (x0 + x1) / 2;
  v = abs(v);
  if (v < 1) v = 1;
  if (v > A_HALF) v = A_HALF;
  g.drawFastVLine(x, A_CY - v, v * 2 + 1, COL_GREEN);
}

static void drawTrackBar(M5Canvas &cv, int i, int v) {
  int x0 = i * CONTENT_W / WAVE_BARS;
  int x1 = (i + 1) * CONTENT_W / WAVE_BARS;
  int x = (x0 + x1) / 2;
  v = abs(v);
  if (v < 1) v = 1;
  if (v > A_HALF) v = A_HALF;
  cv.drawFastVLine(x, A_CY - v, v * 2 + 1, COL_GREEN);
}

static void pushTrackBar(int amp) {
  memmove(waveBars, waveBars + 1, (WAVE_BARS - 1) * sizeof(int8_t));
  memmove(waveBarCounts, waveBarCounts + 1, (WAVE_BARS - 1) * sizeof(uint8_t));
  waveBars[WAVE_BARS - 1] = (int8_t)amp;
  waveBarCounts[WAVE_BARS - 1] = 1;
}

static void drawTrackBars(m5gfx::M5GFX &g) {
  for (int i = 0; i < WAVE_BARS; i++) {
    if (waveBarCounts[i] == 0) continue;
    drawTrackBar(g, i, waveBars[i]);
  }
}

static void drawTrackBars(M5Canvas &cv) {
  for (int i = 0; i < WAVE_BARS; i++) {
    if (waveBarCounts[i] == 0) continue;
    drawTrackBar(cv, i, waveBars[i]);
  }
}

static void drawTrackBarLocal(M5Canvas &cv, int i, int v) {
  int x0 = i * CONTENT_W / WAVE_BARS;
  int x1 = (i + 1) * CONTENT_W / WAVE_BARS;
  int x = (x0 + x1) / 2;
  v = abs(v);
  if (v < 1) v = 1;
  if (v > A_HALF) v = A_HALF;
  cv.drawFastVLine(x, A_CY - WAVE_TOP - v, v * 2 + 1, COL_GREEN);
}

static void drawTrackBarsLocal(M5Canvas &cv) {
  for (int i = 0; i < WAVE_BARS; i++) {
    if (waveBarCounts[i] == 0) continue;
    drawTrackBarLocal(cv, i, waveBars[i]);
  }
}

static void updateLiveWaveVisual(int8_t *dst, const int16_t *src, size_t n, int half, uint8_t smooth, uint8_t decay, uint8_t gain = REC_B_GAIN) {
  if (!dst) return;
  if (!src || n == 0) {
    for (int x = 0; x < CONTENT_W; x++) {
      int old = dst[x];
      if (old > 0) {
        int step = old / decay;
        if (step < 1) step = 1;
        dst[x] = (int8_t)((old > step) ? (old - step) : 0);
      } else if (old < 0) {
        int step = (-old) / decay;
        if (step < 1) step = 1;
        dst[x] = (int8_t)((-old > step) ? (old + step) : 0);
      }
    }
    return;
  }
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += src[i];
  int32_t mean = (int32_t)(sum / (int64_t)n);
  for (int x = 0; x < CONTENT_W; x++) {
    int idx = (int)((uint32_t)x * n / CONTENT_W);
    if (idx >= (int)n) idx = n - 1;
    int32_t centered = (int32_t)src[idx] - mean;
    if (centered > 32767) centered = 32767;
    if (centered < -32768) centered = -32768;
    int target = (int)(centered * gain * half / 32767);
    if (target > half) target = half;
    if (target < -half) target = -half;
    int old = dst[x];
    dst[x] = (int8_t)((old * smooth + target) / (smooth + 1));
  }
}

static void drawLiveWaveVisual(M5Canvas &cv, const int8_t *src, int cy) {
  if (!src) return;
  int px = 0, py = cy - src[0];
  for (int x = 1; x < CONTENT_W; x++) {
    int y = cy - src[x];
    cv.drawLine(px, py, x, y, COL_GREEN);
    px = x;
    py = y;
  }
}

static void drawLiveWaveVisual(m5gfx::M5GFX &g, const int8_t *src, int cy) {
  if (!src) return;
  int px = 0, py = cy - src[0];
  for (int x = 1; x < CONTENT_W; x++) {
    int y = cy - src[x];
    g.drawLine(px, py, x, y, COL_GREEN);
    px = x;
    py = y;
  }
}

// 播放画面: A线=轨道线/Timeline A, B线=监听线/Monitor B(播放缓冲)
static void drawPlaybackCanvas(M5Canvas &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  (void)paused;
  updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(g, playbackLiveWave, B_CY);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  g.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  g.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  g.setTextColor(COL_GREEN, COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, COL_GREEN);
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    g.setTextColor(COL_DIM, COL_BG);
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, COL_DIM);
  }
}

static void drawPlaybackCanvas(m5gfx::M5GFX &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  (void)paused;
  updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(g, playbackLiveWave, B_CY);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  g.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  g.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  g.setTextColor(COL_GREEN, COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, COL_GREEN);
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    g.setTextColor(COL_DIM, COL_BG);
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, COL_DIM);
  }
}

static void drawPlaybackWaveCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN) {
  if (liveWave && liveN > 0) updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  cv.fillScreen(COL_BG);
  cv.drawFastHLine(0, A_CY - WAVE_TOP, CONTENT_W, 0x0440);
  drawTrackBarsLocal(cv);
  cv.drawFastHLine(0, B_CY - WAVE_TOP, CONTENT_W, 0x0440);
  drawLiveWaveVisual(cv, playbackLiveWave, B_CY - WAVE_TOP);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  cv.drawFastVLine(headX, 0, WAVE_CY - WAVE_TOP, COL_WHITE);
}

static void drawPlaybackWaveDirect(m5gfx::M5GFX &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN) {
  if (liveWave && liveN > 0) updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(g, playbackLiveWave, B_CY);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  g.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);
}

static void drawPlaybackChrome(m5gfx::M5GFX &g, uint32_t played, uint32_t dataSize, uint32_t deleteHeldMs = 0) {
  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  g.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, COL_GREEN);
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, COL_DIM);
  }
}

static void clearPlaybackDeleteChrome(m5gfx::M5GFX &g) {
  g.fillRect(112, 119, 126, 15, COL_BG);
}

static void drawPlaybackAction(M5Canvas &cv, const char *msg, uint16_t col = COL_GREEN) {
  (void)cv;
  (void)msg;
  (void)col;
}

static bool enqueueUploadMounted(int recNum, uint8_t kind);
static bool uploadCancelMounted(int recNum = 0);
static uint8_t validateUploadConfigMounted();
static const char *uploadStatusLabel(uint8_t status);
static bool extractJsonStringValue(const String &json, const char *key, char *out, size_t n);

// ---------- 回放界面: 播放完自动回列表, 回车暂停/继续, 退格回列表, ;/.切换, +/- 音量, 长按Del删除, 空格去录音 ----------
// 返回 R_BACK(返回上一层), R_LIST(回列表), R_RECORD(去录音) 或 R_DELETE(删除当前录音)
int playbackScreen(const char *path, int recNum, int prevRec, int nextRec) {
  pauseUploadForMedia();
  auto &d = M5Cardputer.Display;
  if (!sdMount()) { showMsg("播放", "SD 读取失败", COL_RED); return R_LIST; }
  File f = SD.open(path, FILE_READ);
  if (!f) { SD.end(); showMsg("播放", "打不开文件", COL_RED); return R_LIST; }
  uint32_t total = f.size();
  if (total <= 44) { f.close(); SD.end(); showMsg("播放", "文件为空", COL_RED); return R_LIST; }
  uint32_t dataSize = playableWavDataBytes(f);
  if (dataSize == 0) { f.close(); SD.end(); showMsg("播放", "文件无效", COL_RED); return R_LIST; }
  bool isHotkeyPlayback = hotkeyOf(recNum) != 0;
  f.seek(44);
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(playbackLiveWave, 0, sizeof(playbackLiveWave));

  d.fillScreen(COL_BG);
  M5Canvas waveCv(&d);
  bool useWaveSprite = waveCv.createSprite(CONTENT_W, WAVE_H) != nullptr;
  M5Canvas bottomCv(&d);
  bool useBottomSprite = useWaveSprite && bottomCv.createSprite(CONTENT_W, CHROME_H) != nullptr;
  M5Canvas cv(&d);
  bool useSprite = !useWaveSprite && cv.createSprite(CONTENT_W, d.height()) != nullptr;
  const uint32_t pbFrameMs = (useWaveSprite || useSprite) ? PB_UI_FRAME_MS : 90;
  g_mediaBusy = true;

  // 静态部分(画一次): 顶栏
  auto drawStatic = [&]() {
    // 文件编号 (左上小字)
    char title[16];
    snprintf(title, sizeof(title), "REC_%04d", recNum);
    char hk = hotkeyOf(recNum);
    if (useSprite) {
      cv.fillScreen(COL_BG);
      drawStatusTitleNoLine(cv, title, hk ? COL_DIM : COL_GREEN);
      if (hk) {
        char keyLabel[8];
        snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
        drawDseg14Text(cv, dseg14TextWidth(title) + 10, 1, keyLabel, COL_GREEN);
      }
      cv.drawFastHLine(0, WAVE_BOT + 1, CONTENT_W, 0x0820);
    } else {
      d.fillScreen(COL_BG);
      drawStatusTitleNoLine(d, title, hk ? COL_DIM : COL_GREEN);
      if (hk) {
        char keyLabel[8];
        snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
        drawDseg14Text(d, dseg14TextWidth(title) + 10, 1, keyLabel, COL_GREEN);
      }
      d.drawFastHLine(0, WAVE_BOT + 1, CONTENT_W, 0x0820);
    }
    if (useSprite) cv.pushSprite(0, 0);
  };
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;
  bool paused = false;
  size_t playbackVisualN = 0;
  uint32_t lastPbChromeSec = 0xFFFFFFFFUL;
  uint32_t lastPbChromeHeldBucket = 0xFFFFFFFFUL;
  auto deleteHeldMs = [&]() -> uint32_t {
    if (delHoldStart == 0) return 0;
    uint32_t held = millis() - delHoldStart;
    if (held < DELETE_HINT_MS) return 0;
    return held > DELETE_HOLD_MS ? DELETE_HOLD_MS : held;
  };
  auto drawProgress = [&](uint32_t played, int16_t *liveWave = nullptr, size_t liveN = 0) {
    if (useWaveSprite) {
      if (liveWave && liveN > 0) {
        if (liveN > PB_N) liveN = PB_N;
        memcpy(playbackVisualBuf, liveWave, liveN * sizeof(int16_t));
        playbackVisualN = liveN;
      }
      int16_t *visualWave = playbackVisualN ? playbackVisualBuf : nullptr;
      drawPlaybackWaveCanvas(waveCv, played, dataSize, visualWave, playbackVisualN);
      waveCv.pushSprite(0, WAVE_TOP);
      uint32_t held = deleteHeldMs();
      uint32_t chromeSec = (played / 2) / REC_RATE;
      uint32_t heldBucket = held / 60;
      if (chromeSec != lastPbChromeSec || heldBucket != lastPbChromeHeldBucket) {
        if (useBottomSprite) {
          drawPlaybackBottomCanvas(bottomCv, played, dataSize, held);
          bottomCv.pushSprite(0, CHROME_TOP);
        } else {
          drawPlaybackChrome(d, played, dataSize, held);
        }
        lastPbChromeSec = chromeSec;
        lastPbChromeHeldBucket = heldBucket;
      }
    } else if (useSprite) {
      drawPlaybackCanvas(cv, played, dataSize, liveWave, liveN, paused, deleteHeldMs());
      cv.pushSprite(0, 0);
    } else {
      drawPlaybackWaveDirect(d, played, dataSize, liveWave, liveN);
      uint32_t held = deleteHeldMs();
      uint32_t chromeSec = (played / 2) / REC_RATE;
      uint32_t heldBucket = held / 60;
      if (chromeSec != lastPbChromeSec || heldBucket != lastPbChromeHeldBucket) {
        drawPlaybackChrome(d, played, dataSize, held);
        lastPbChromeSec = chromeSec;
        lastPbChromeHeldBucket = heldBucket;
      }
    }
  };

  bool stop = false, playDone = false;
  int ret = R_BACK;
  uint32_t played = 0, remaining = dataSize;
  uint32_t lastSeek = 0;
  uint32_t lastPreviewDraw = 0;
  uint32_t lastProgressDraw = 0;
  uint32_t fadeBytesLeft = REC_RATE * 2 * 80 / 1000;
  uint8_t previewBar = 0;
  int pi = 0;

  drawStatic();
  drawProgress(0);
  lastProgressDraw = millis();
  speakerOn();
  applyPlaybackVolumeForRec(recNum);
  bool ignoreKeysUntilRelease = M5Cardputer.Keyboard.isPressed();

  while (!stop) {
    M5Cardputer.update();
    if (ignoreKeysUntilRelease) {
      if (!M5Cardputer.Keyboard.isPressed()) ignoreKeysUntilRelease = false;
    } else {
      if (keyDel()) {
        if (isHotkeyPlayback) {
          drawPlaybackAction(cv, "STOP", COL_DIM);
          ret = R_LIST;
          stop = true;
          waitRelease();
          break;
        }
        if (delHoldStart == 0) { delHoldStart = millis(); lastDelDraw = 0; }
        uint32_t held = millis() - delHoldStart;
        if (held >= DELETE_HOLD_MS) { ret = R_DELETE; stop = true; }
        else if (held >= DELETE_HINT_MS && millis() - lastDelDraw > 60) { lastDelDraw = millis(); drawProgress(played); lastProgressDraw = lastDelDraw; }
      } else if (delHoldStart != 0) {
        delHoldStart = 0;
        if (useWaveSprite) {
          clearPlaybackDeleteChrome(d);
          lastPbChromeHeldBucket = 0xFFFFFFFFUL;
        }
        drawPlaybackAction(cv, "BACK", COL_DIM);
        ret = R_LIST; stop = true;                         // 退格短按=回上一层
      }
      if (stop) break;

      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        int hk = pressedHotkeyRec();
        if (hk > 0) {
          drawPlaybackAction(cv, "PLAY", COL_GREEN);
          g_listMode = REC_SHORTCUT;
          g_nextPlay = hk; ret = R_PLAY; stop = true;
        }   // 按到别的绑定键=立刻覆盖播放
        else if (keyEsc()) { drawPlaybackAction(cv, "SLEEP", COL_DIM); ret = R_BACK; stop = true; }       // Esc=息屏
        else if (keySpace()) { drawPlaybackAction(cv, "REC", COL_RED); ret = R_RECORD; stop = true; }   // 空格=去录音
        else if (keyUp() && prevRec > 0) { drawPlaybackAction(cv, "PREV", COL_GREEN); g_nextPlay = prevRec; ret = R_PLAY; stop = true; }
        else if (keyDown() && nextRec > 0) { drawPlaybackAction(cv, "NEXT", COL_GREEN); g_nextPlay = nextRec; ret = R_PLAY; stop = true; }
        else if (keyTab() && keyUpload()) {
          drawPlaybackAction(cv, "LIST", COL_DIM);
          waitRelease();
        }
        else if (keyUpload()) {
          drawPlaybackAction(cv, "LIST", COL_DIM);
          waitRelease();
        }
        else if (keyEnter()) {
          paused = !paused;
          if (paused) M5Cardputer.Speaker.stop();
          drawProgress(played);
          lastProgressDraw = millis();
          drawPlaybackAction(cv, paused ? "PAUSE" : "PLAY", paused ? COL_DIM : COL_GREEN);
          waitRelease();
        }
        else if (keyVolUp()) {
          if (keyAlt()) {
            adjustRecVolumeDelta(recNum, 1);
            applyPlaybackVolumeForRec(recNum);
            drawPlaybackAction(cv, "V+", COL_GREEN);
          } else {
            adjustPlayVolume(25);
            applyPlaybackVolumeForRec(recNum);
            if (useSprite) drawCanvasVolumeToast(cv, COL_GREEN);
            else drawVolumeToast(COL_GREEN);
          }
        }
        else if (keyVolDn()) {
          if (keyAlt()) {
            adjustRecVolumeDelta(recNum, -1);
            applyPlaybackVolumeForRec(recNum);
            drawPlaybackAction(cv, "V-", COL_GREEN);
          } else {
            adjustPlayVolume(-25);
            applyPlaybackVolumeForRec(recNum);
            if (useSprite) drawCanvasVolumeToast(cv, COL_GREEN);
            else drawVolumeToast(COL_GREEN);
          }
        }
        else if (keyBrightUp()) {
          adjustBrightness(1);
          if (useSprite) drawCanvasBrightnessToast(cv, COL_GREEN);
          else drawBrightnessToast(COL_GREEN);
        }
        else if (keyBrightDn()) {
          adjustBrightness(-1);
          if (useSprite) drawCanvasBrightnessToast(cv, COL_GREEN);
          else drawBrightnessToast(COL_GREEN);
        }
      }
    }
    if (stop) break;
    if (paused) { delay(8); continue; }

    bool seekLeft = keyLeft();
    bool seekRight = keyRight();
    if ((seekLeft || seekRight) && millis() - lastSeek > 120) {
      lastSeek = millis();
      M5Cardputer.Speaker.stop();
      uint32_t step = REC_RATE * 2;  // 约 1 秒 PCM 数据
      if (seekLeft) played = (played > step) ? (played - step) : 0;
      if (seekRight) played = (played + step < dataSize) ? (played + step) : dataSize;
      played &= ~1U;
      remaining = dataSize - played;
      f.seek(44 + played);
      playbackVisualN = 0;
      memset(playbackLiveWave, 0, sizeof(playbackLiveWave));
      fadeBytesLeft = REC_RATE * 2 * 80 / 1000;
      drawProgress(played);
      lastProgressDraw = millis();
      playSeekFeedback(seekRight && !seekLeft);
      delay(8);
      continue;
    }
    if (remaining == 0) {
      if (M5Cardputer.Speaker.isPlaying(0) == 0) {
        if (!playDone) {
          playDone = true;
          drawProgress(dataSize);   // 显示 100% 最终状态
        }
        delay(80);
        if (delHoldStart != 0 && keyDel()) {
          g_carryDeleteRec = recNum;
          g_carryDeleteStart = delHoldStart;
        }
        ret = R_LIST;
        stop = true;
        continue;
      }
      delay(8); continue;
    }
    if (M5Cardputer.Speaker.isPlaying(0) >= 2) {
      uint32_t now = millis();
      if (now - lastPreviewDraw > pbFrameMs) {
        lastPreviewDraw = now;
        previewPlaybackWaveStep(f, dataSize, previewBar, 1);
        drawProgress(played);
        lastProgressDraw = lastPreviewDraw;
      }
      delay(1);
      continue;
    }
    size_t want = remaining < sizeof(pbBuf[pi]) ? remaining : sizeof(pbBuf[pi]);
    int got = f.read((uint8_t *)pbBuf[pi], want);
    if (got <= 0) { remaining = 0; continue; }
    remaining -= got;
    int16_t *liveWave = pbBuf[pi];
    size_t liveN = got / 2;
    if (fadeBytesLeft > 0) {
      uint32_t fadeBytes = REC_RATE * 2 * 80 / 1000;
      for (size_t i = 0; i < liveN && fadeBytesLeft > 0; i++) {
        uint32_t done = fadeBytes - fadeBytesLeft;
        int32_t gain = (int32_t)(done * 256 / fadeBytes);
        liveWave[i] = (int16_t)(((int32_t)liveWave[i] * gain) >> 8);
        fadeBytesLeft = (fadeBytesLeft > 2) ? (fadeBytesLeft - 2) : 0;
      }
    }
    M5Cardputer.Speaker.playRaw(liveWave, liveN, REC_RATE, false, 1, 0, false);
    pi ^= 1;
    played += got;
    uint32_t now = millis();
    if (now - lastProgressDraw >= pbFrameMs) {
      lastProgressDraw = now;
      drawProgress(played, liveWave, liveN);
    }
  }
  if (useBottomSprite) bottomCv.deleteSprite();
  if (useWaveSprite) waveCv.deleteSprite();
  if (useSprite) cv.deleteSprite();
  M5Cardputer.Speaker.stop();
  f.close();
  if (recVolumeDirty) saveRecVolumeStateMounted();
  SD.end();
  g_mediaBusy = false;
  return ret;
}

void noiseReduce(const char *path);
static bool suppressKeyFriction(const char *path, uint32_t dataBytes, bool abortOnKey = false);
void listFlow(int sel);
int recordingScreen();
static void deleteRecording(int recNum);

// 播放流程: 支持"播放中按别的绑定键 -> 立刻切到那条"(覆盖). 返回 R_BACK/R_LIST/R_RECORD.
int playFlow(int recNum) {
  while (true) {
    if (recCount <= 0 || recListIndexOf(recNum) < 0) scanRecordings();
    int prevRec = 0, nextRec = 0;
    int idx = recListIndexOf(recNum);
    if (idx >= 0) {
      if (recVisibleInMode(recNum, g_listMode)) {
        int pi = prevVisibleIndex(idx, g_listMode);
        int ni = nextVisibleIndex(idx, g_listMode);
        if (pi != idx) prevRec = recList[pi];
        if (ni != idx) nextRec = recList[ni];
      } else {
        if (idx > 0) prevRec = recList[idx - 1];
        if (idx + 1 < recCount) nextRec = recList[idx + 1];
      }
    }
    char p[40]; recordingPathForRec(recNum, p, sizeof(p));
    int r = playbackScreen(p, recNum, prevRec, nextRec);
    if (r == R_PLAY) { recNum = g_nextPlay; continue; }
    if (r == R_DELETE) { g_listReturnRec = 0; deleteRecording(recNum); return R_LIST; }
    if (r == R_LIST) g_listReturnRec = recNum;
    return r;
  }
}

void afterRecordingFlow(int recNum) {
  int action = g_afterRecord;
  if (recNum <= 0) {
    if (action == R_LIST) { listFlow(0); return; }
    return;
  }
  if (action == R_BACK) return;
  while (recNum > 0) {
    if (action == R_NOISE) {
      char p[40]; recordingPathForRec(recNum, p, sizeof(p));
      noiseReduce(p);
      action = R_LIST;
      continue;
    }
    listFlow(recNum);
    return;
  }
}

// ---------- 系统层上传服务: 录音应用只入队, 盒子负责 Wi-Fi / 调度 / 重试 ----------
struct UploadConfig {
  char ssid[33];
  char password[65];
  char ssid2[33];
  char password2[65];
  char url[129];
  char token[65];
  char device[25];
  char ntp[65];
  int16_t tzMinutes;
  bool syncEnabled;
};
static UploadConfig uploadCfg;

static void strTrim(char *s) {
  if (!s) return;
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
  char *p = s;
  while (*p == ' ' || *p == '\t') p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
}

static void copyCfgValue(char *dst, size_t dstLen, const char *src) {
  if (!dst || dstLen == 0) return;
  if (!src) { dst[0] = 0; return; }
  strlcpy(dst, src, dstLen);
  strTrim(dst);
}

static bool loadUploadConfig() {
  memset(&uploadCfg, 0, sizeof(uploadCfg));
  strlcpy(uploadCfg.device, "cardputer-001", sizeof(uploadCfg.device));
  strlcpy(uploadCfg.ntp, "pool.ntp.org", sizeof(uploadCfg.ntp));
  uploadCfg.tzMinutes = 480;
  uploadCfg.syncEnabled = true;
  if (!SD.exists(UPLOAD_CONFIG_PATH)) return false;
  File f = SD.open(UPLOAD_CONFIG_PATH, FILE_READ);
  if (!f) return false;
  char line[180];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    strTrim(line);
    if (line[0] == 0 || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    strTrim(line);
    strTrim(eq);
    if (strcmp(line, "ssid") == 0) copyCfgValue(uploadCfg.ssid, sizeof(uploadCfg.ssid), eq);
    else if (strcmp(line, "password") == 0) copyCfgValue(uploadCfg.password, sizeof(uploadCfg.password), eq);
    else if (strcmp(line, "ssid2") == 0) copyCfgValue(uploadCfg.ssid2, sizeof(uploadCfg.ssid2), eq);
    else if (strcmp(line, "password2") == 0) copyCfgValue(uploadCfg.password2, sizeof(uploadCfg.password2), eq);
    else if (strcmp(line, "sync") == 0) uploadCfg.syncEnabled = atoi(eq) != 0;
    else if (strcmp(line, "url") == 0) copyCfgValue(uploadCfg.url, sizeof(uploadCfg.url), eq);
    else if (strcmp(line, "token") == 0) copyCfgValue(uploadCfg.token, sizeof(uploadCfg.token), eq);
    else if (strcmp(line, "device") == 0) copyCfgValue(uploadCfg.device, sizeof(uploadCfg.device), eq);
    else if (strcmp(line, "ntp") == 0) copyCfgValue(uploadCfg.ntp, sizeof(uploadCfg.ntp), eq);
    else if (strcmp(line, "tz") == 0) uploadCfg.tzMinutes = (int16_t)(atoi(eq) * 60);
    else if (strcmp(line, "tz_minutes") == 0) uploadCfg.tzMinutes = (int16_t)atoi(eq);
  }
  f.close();
  return !uploadCfg.syncEnabled || ((uploadCfg.ssid[0] || uploadCfg.ssid2[0]) && uploadCfg.url[0] && uploadCfg.token[0]);
}

static bool uploadLineHasRec(const char *path, int recNum) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  char line[32];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (atoi(line) == recNum) { f.close(); return true; }
  }
  f.close();
  return false;
}

static bool uploadRemoveDoneMounted(int recNum) {
  if (recNum <= 0) return false;
  File in = SD.open(UPLOAD_DONE_PATH, FILE_READ);
  if (!in) return false;
  const char *tmp = "/UPLOAD/done.tmp";
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) {
    in.close();
    SD.remove(tmp);
    return false;
  }
  bool removed = false;
  char line[48];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    char trimmed[48];
    snprintf(trimmed, sizeof(trimmed), "%s", line);
    strTrim(trimmed);
    if (!trimmed[0]) continue;
    if (atoi(trimmed) == recNum) {
      removed = true;
      continue;
    }
    out.printf("%s\n", trimmed);
  }
  in.close();
  out.close();
  SD.remove(UPLOAD_DONE_PATH);
  bool committed = SD.rename(tmp, UPLOAD_DONE_PATH);
  if (!committed) SD.remove(tmp);
  if (removed && committed) setUploadDone(recNum, false);
  return removed && committed;
}

static bool uploadRemoveLineByRecMounted(const char *path, int recNum, uint8_t *bits = nullptr) {
  if (recNum <= 0) return false;
  File in = SD.open(path, FILE_READ);
  if (!in) return false;
  const char *tmp = "/UPLOAD/state.tmp";
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) {
    in.close();
    SD.remove(tmp);
    return false;
  }
  bool removed = false;
  char line[128];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    char trimmed[128];
    snprintf(trimmed, sizeof(trimmed), "%s", line);
    strTrim(trimmed);
    if (!trimmed[0]) continue;
    if (atoi(trimmed) == recNum) {
      removed = true;
      continue;
    }
    out.printf("%s\n", trimmed);
  }
  in.close();
  out.close();
  SD.remove(path);
  bool committed = SD.rename(tmp, path);
  if (!committed) SD.remove(tmp);
  if (removed && committed && bits) setBit(bits, recNum, false);
  return removed && committed;
}

static void uploadClearNonDoneStateMounted(int recNum) {
  uploadRemoveLineByRecMounted(UPLOAD_PENDING_PATH, recNum, uploadPendingBits);
  uploadRemoveLineByRecMounted(UPLOAD_MODEL_ERR_PATH, recNum, uploadModelErrBits);
  uploadRemoveLineByRecMounted(UPLOAD_JOB_ERR_PATH, recNum, uploadJobErrBits);
}

static bool enqueueUploadMounted(int recNum, uint8_t kind) {
  if (recNum <= 0) return false;
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  if (uploadLineHasRec(UPLOAD_DONE_PATH, recNum) && !uploadRemoveDoneMounted(recNum)) return false;
  uploadClearNonDoneStateMounted(recNum);
  if (uploadLineHasRec(UPLOAD_QUEUE_PATH, recNum)) {
    setUploadQueued(recNum, true);
    return true;
  }
  File f = SD.open(UPLOAD_QUEUE_PATH, FILE_APPEND);
  if (!f) return false;
  f.printf("%d %u\n", recNum, (unsigned)kind);
  f.close();
  setUploadQueued(recNum, true);
  return true;
}

static bool uploadReadFirstJob(int &recNum, uint8_t &kind) {
  recNum = 0;
  kind = REC_NORMAL;
  File f = SD.open(UPLOAD_QUEUE_PATH, FILE_READ);
  if (!f) return false;
  char line[32];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    strTrim(line);
    if (!line[0]) continue;
    int k = 0;
    if (sscanf(line, "%d %d", &recNum, &k) >= 1 && recNum > 0) {
      if (k >= REC_NORMAL && k <= REC_IMPORTANT) kind = (uint8_t)k;
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

static void uploadDropFirstJob() {
  File in = SD.open(UPLOAD_QUEUE_PATH, FILE_READ);
  if (!in) return;
  const char *tmp = "/UPLOAD/queue.tmp";
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) {
    in.close();
    SD.remove(tmp);
    return;
  }
  bool dropped = false;
  int droppedRec = 0;
  char line[48];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (!dropped) {
      dropped = true;
      droppedRec = atoi(line);
      continue;
    }
    out.printf("%s\n", line);
  }
  in.close();
  out.close();
  SD.remove(UPLOAD_QUEUE_PATH);
  bool committed = SD.rename(tmp, UPLOAD_QUEUE_PATH);
  if (!committed) SD.remove(tmp);
  if (droppedRec > 0 && committed) setUploadQueued(droppedRec, false);
}

static bool uploadRemoveQueuedMounted(int recNum) {
  if (recNum <= 0) return false;
  File in = SD.open(UPLOAD_QUEUE_PATH, FILE_READ);
  if (!in) return false;
  const char *tmp = "/UPLOAD/queue.tmp";
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) {
    in.close();
    SD.remove(tmp);
    return false;
  }
  bool removed = false;
  char line[48];
  while (in.available()) {
    int n = in.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    char trimmed[48];
    snprintf(trimmed, sizeof(trimmed), "%s", line);
    strTrim(trimmed);
    if (!trimmed[0]) continue;
    if (atoi(trimmed) == recNum) {
      removed = true;
      continue;
    }
    out.printf("%s\n", trimmed);
  }
  in.close();
  out.close();
  SD.remove(UPLOAD_QUEUE_PATH);
  bool committed = SD.rename(tmp, UPLOAD_QUEUE_PATH);
  if (!committed) SD.remove(tmp);
  if (removed && committed) setUploadQueued(recNum, false);
  return removed && committed;
}

static bool uploadCancelMounted(int recNum) {
  if (recNum <= 0) {
    uint8_t kind = REC_NORMAL;
    uploadReadFirstJob(recNum, kind);
  }
  bool removed = uploadRemoveQueuedMounted(recNum);
  if (removed) {
    g_uploadCancelRequested = false;
    g_uploadPausedForInput = false;
    if (g_uploadActiveRec == recNum) g_uploadActiveRec = 0;
    if (g_uploadStatus == UPSTAT_UPLOADING || g_uploadStatus == UPSTAT_QUEUED) g_uploadStatus = UPSTAT_ABORTED;
  }
  return removed;
}

static void uploadMarkDone(int recNum) {
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  uploadClearNonDoneStateMounted(recNum);
  if (!uploadLineHasRec(UPLOAD_DONE_PATH, recNum)) {
    File f = SD.open(UPLOAD_DONE_PATH, FILE_APPEND);
    if (!f) return;
    f.printf("%d\n", recNum);
    f.close();
  }
  setUploadDone(recNum, true);
  setUploadQueued(recNum, false);
}

static void uploadMarkPending(int recNum, const char *jobId) {
  if (recNum <= 0 || !jobId || !jobId[0]) return;
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  uploadRemoveLineByRecMounted(UPLOAD_PENDING_PATH, recNum, uploadPendingBits);
  uploadRemoveLineByRecMounted(UPLOAD_MODEL_ERR_PATH, recNum, uploadModelErrBits);
  uploadRemoveLineByRecMounted(UPLOAD_JOB_ERR_PATH, recNum, uploadJobErrBits);
  File f = SD.open(UPLOAD_PENDING_PATH, FILE_APPEND);
  if (!f) return;
  f.printf("%d %s\n", recNum, jobId);
  f.close();
  setUploadPending(recNum, true);
  setUploadDone(recNum, false);
  setUploadQueued(recNum, false);
}

static void uploadMarkError(int recNum, bool modelError) {
  if (recNum <= 0) return;
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  uploadRemoveLineByRecMounted(UPLOAD_PENDING_PATH, recNum, uploadPendingBits);
  const char *path = modelError ? UPLOAD_MODEL_ERR_PATH : UPLOAD_JOB_ERR_PATH;
  uint8_t *bits = modelError ? uploadModelErrBits : uploadJobErrBits;
  uploadRemoveLineByRecMounted(path, recNum, bits);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  f.printf("%d\n", recNum);
  f.close();
  setBit(bits, recNum, true);
  setUploadDone(recNum, false);
  setUploadQueued(recNum, false);
}

static void uploadForgetRecordMounted(int recNum) {
  if (recNum <= 0) return;
  uploadRemoveQueuedMounted(recNum);
  uploadRemoveDoneMounted(recNum);
  uploadClearNonDoneStateMounted(recNum);
  setUploadQueued(recNum, false);
  setUploadDone(recNum, false);
  setUploadPending(recNum, false);
  setUploadModelErr(recNum, false);
  setUploadJobErr(recNum, false);
  if (recNum > 0 && recNum <= MAX_REC) {
    recVolumeDelta[recNum - 1] = 0;
    recVolumeDirty = true;
  }
}

static bool saveUploadConfigMounted() {
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  File f = SD.open(UPLOAD_CONFIG_PATH, FILE_WRITE);
  if (!f) return false;
  f.printf("sync=%d\n", uploadCfg.syncEnabled ? 1 : 0);
  f.printf("ssid=%s\n", uploadCfg.ssid);
  f.printf("password=%s\n", uploadCfg.password);
  f.printf("ssid2=%s\n", uploadCfg.ssid2);
  f.printf("password2=%s\n", uploadCfg.password2);
  f.printf("url=%s\n", uploadCfg.url);
  f.printf("token=%s\n", uploadCfg.token);
  f.printf("device=%s\n", uploadCfg.device[0] ? uploadCfg.device : "cardputer-001");
  f.printf("ntp=%s\n", uploadCfg.ntp[0] ? uploadCfg.ntp : "pool.ntp.org");
  f.printf("tz_minutes=%d\n", (int)uploadCfg.tzMinutes);
  f.close();
  return true;
}

#if UPLOAD_WIFI_ENABLED
static bool boxClockSynced = false;
static uint32_t lastClockSyncAttemptMs = 0;

static bool boxClockHasTime() {
  time_t now = time(nullptr);
  return now > 1700000000;  // After 2023-11, avoids accepting the boot default epoch.
}

static bool boxSyncClockIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return boxClockHasTime();
  if (boxClockHasTime()) {
    boxClockSynced = true;
    return true;
  }
  uint32_t nowMs = millis();
  if (lastClockSyncAttemptMs != 0 && nowMs - lastClockSyncAttemptMs < 120000) return false;
  lastClockSyncAttemptMs = nowMs;
  long gmtOffsetSec = (long)uploadCfg.tzMinutes * 60L;
  const char *ntp = uploadCfg.ntp[0] ? uploadCfg.ntp : "pool.ntp.org";
  configTime(gmtOffsetSec, 0, ntp, "time.cloudflare.com", "time.google.com");
  uint32_t start = millis();
  while (!boxClockHasTime() && millis() - start < 3500) {
    M5Cardputer.update();
    if (keyUploadAbort()) return false;
    delay(100);
  }
  boxClockSynced = boxClockHasTime();
  return boxClockSynced;
}

static bool boxFormatNow(char *buf, size_t n) {
  if (!buf || n == 0) return false;
  buf[0] = 0;
  if (!boxClockHasTime()) return false;
  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  return strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmNow) > 0;
}

static void wifiPowerDown() {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(60);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_wifi_deinit();
  delay(140);
  boxClockSynced = false;
}

static void pauseUploadForMedia() {
  g_uploadActiveAnnounced = false;
  g_uploadActiveRec = 0;
  if (g_uploadStatus == UPSTAT_UPLOADING) g_uploadStatus = UPSTAT_QUEUED;
#if UPLOAD_WIFI_ENABLED
  wifiPowerDown();
#endif
}

static bool tryWifiProfile(const char *ssid, const char *password) {
  if (!ssid || !ssid[0]) return false;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.begin(ssid, password ? password : "");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 4500) {
    M5Cardputer.update();
    if (keyUploadAbort()) return false;
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

static void boxSaveRecordedAtMounted(int recNum) {
  if (recNum <= 0) return;
  char ts[24];
  if (!boxFormatNow(ts, sizeof(ts))) return;
  if (!SD.exists(UPLOAD_DIR)) SD.mkdir(UPLOAD_DIR);
  File f = SD.open(UPLOAD_RECORDED_AT_PATH, FILE_APPEND);
  if (!f) return;
  f.printf("%d %s\n", recNum, ts);
  f.close();
}

static bool boxReadRecordedAtMounted(int recNum, char *out, size_t n) {
  if (!out || n == 0) return false;
  out[0] = 0;
  File f = SD.open(UPLOAD_RECORDED_AT_PATH, FILE_READ);
  if (!f) return false;
  char line[48];
  bool found = false;
  while (f.available()) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[len] = 0;
    strTrim(line);
    int id = atoi(line);
    char *sp = strchr(line, ' ');
    if (id == recNum && sp && sp[1]) {
      copyCfgValue(out, n, sp + 1);
      found = true;
    }
  }
  f.close();
  return found;
}

static bool ensureWifiConnected() {
  if (!uploadCfg.syncEnabled) {
    wifiPowerDown();
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) {
    boxSyncClockIfNeeded();
    return true;
  }
  if (!tryWifiProfile(uploadCfg.ssid, uploadCfg.password) &&
      !tryWifiProfile(uploadCfg.ssid2, uploadCfg.password2)) {
    wifiPowerDown();
    return false;
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  boxSyncClockIfNeeded();
  return true;
}

static bool parseHttpUrl(const char *url, char *host, size_t hostLen, uint16_t &port, char *path, size_t pathLen) {
  const char *prefix = "http://";
  if (strncmp(url, prefix, 7) != 0) return false;
  const char *p = url + 7;
  const char *slash = strchr(p, '/');
  const char *hostEnd = slash ? slash : (p + strlen(p));
  const char *colon = nullptr;
  for (const char *q = p; q < hostEnd; q++) {
    if (*q == ':') { colon = q; break; }
  }
  size_t hLen = (colon ? colon : hostEnd) - p;
  if (hLen == 0 || hLen >= hostLen) return false;
  memcpy(host, p, hLen);
  host[hLen] = 0;
  port = colon ? (uint16_t)atoi(colon + 1) : 80;
  if (port == 0) port = 80;
  strlcpy(path, slash ? slash : "/", pathLen);
  return true;
}

static uint8_t imaAdpcmEncodeNibble(int16_t sample, int16_t &predictor, uint8_t &index) {
  int step = IMA_STEP_TABLE[index];
  int diff = (int)sample - (int)predictor;
  uint8_t nibble = 0;
  if (diff < 0) {
    nibble = 8;
    diff = -diff;
  }

  int vpdiff = step >> 3;
  if (diff >= step) {
    nibble |= 4;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    nibble |= 2;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    nibble |= 1;
    vpdiff += step;
  }

  int next = (nibble & 8) ? ((int)predictor - vpdiff) : ((int)predictor + vpdiff);
  if (next > 32767) next = 32767;
  if (next < -32768) next = -32768;
  predictor = (int16_t)next;
  int nextIndex = (int)index + IMA_INDEX_TABLE[nibble & 0x0f];
  if (nextIndex < 0) nextIndex = 0;
  if (nextIndex > 88) nextIndex = 88;
  index = (uint8_t)nextIndex;
  return nibble & 0x0f;
}

static int16_t readLe16(const uint8_t *p) {
  return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool makeJobStatusPath(const char *uploadPath, const char *jobId, char *out, size_t n) {
  if (!uploadPath || !jobId || !jobId[0] || !out || n == 0) return false;
  const char *suffix = "/upload";
  size_t len = strlen(uploadPath);
  size_t suffixLen = strlen(suffix);
  out[0] = 0;
  if (len >= suffixLen && strcmp(uploadPath + len - suffixLen, suffix) == 0) {
    size_t baseLen = len - suffixLen;
    if (baseLen + strlen("/jobs/") + strlen(jobId) >= n) return false;
    memcpy(out, uploadPath, baseLen);
    out[baseLen] = 0;
    strlcat(out, "/jobs/", n);
    strlcat(out, jobId, n);
    return true;
  }
  if (strlen("/jobs/") + strlen(jobId) >= n) return false;
  strlcpy(out, "/jobs/", n);
  strlcat(out, jobId, n);
  return true;
}

static String readHttpBody(WiFiClient &client, int &code, uint32_t timeoutMs = 5000, bool abortOnKey = true) {
  code = 0;
  int contentLength = -1;
  String body;
  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < timeoutMs) {
    M5Cardputer.update();
    if (abortOnKey && keyUploadAbort()) return body;
    delay(20);
  }
  if (client.available()) {
    String status = client.readStringUntil('\n');
    int sp = status.indexOf(' ');
    if (sp >= 0) code = status.substring(sp + 1).toInt();
    while (client.connected() || client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("content-length:")) contentLength = lower.substring(15).toInt();
    }
    start = millis();
    while ((client.connected() || client.available()) && millis() - start < timeoutMs) {
      while (client.available()) {
        char c = (char)client.read();
        if (body.length() < 768) body += c;
        if (contentLength > 0 && body.length() >= (unsigned)contentLength) return body;
      }
      M5Cardputer.update();
      if (abortOnKey && keyUploadAbort()) return body;
      delay(5);
    }
  }
  return body;
}

static int readHttpStatusCodeOnly(WiFiClient &client, uint32_t timeoutMs = 5000) {
  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < timeoutMs) {
    M5Cardputer.update();
    if (keyUploadAbort()) return 0;
    delay(20);
  }
  if (!client.available()) return 0;
  char line[96];
  int n = client.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n > 0 ? n : 0] = 0;
  char *sp = strchr(line, ' ');
  return sp ? atoi(sp + 1) : 0;
}

static uint8_t uploadCheckJobStatusMounted(const char *host, uint16_t port, const char *hostHeader, const char *uploadPath, const char *jobId) {
  char jobPath[180];
  if (!makeJobStatusPath(uploadPath, jobId, jobPath, sizeof(jobPath))) return UPSTAT_BAD_URL;
  WiFiClient client;
  client.setTimeout(5000);
  if (!client.connect(host, port, 3000)) return UPSTAT_HTTP_ERR;
  client.setNoDelay(true);
  client.printf("GET %s HTTP/1.1\r\n", jobPath);
  client.printf("Host: %s\r\n", hostHeader);
  client.print("Connection: close\r\n\r\n");
  int code = 0;
  String body = readHttpBody(client, code, 6000, false);
  client.stop();
  if (code < 200 || code >= 300) return UPSTAT_HTTP_ERR;
  char status[32];
  if (!extractJsonStringValue(body, "status", status, sizeof(status))) return UPSTAT_PROCESSING;
  if (strcmp(status, "done") == 0 || strcmp(status, "sent_to_flomo") == 0) return UPSTAT_DONE;
  if (strstr(status, "deepseek_failed") || strstr(status, "chat_reply_failed")) return UPSTAT_MODEL_ERR;
  if (strstr(status, "failed")) return UPSTAT_JOB_ERR;
  return UPSTAT_PROCESSING;
}

static bool uploadReadFirstPending(int &recNum, char *jobId, size_t jobIdLen) {
  recNum = 0;
  if (jobId && jobIdLen) jobId[0] = 0;
  File f = SD.open(UPLOAD_PENDING_PATH, FILE_READ);
  if (!f) return false;
  char line[140];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    strTrim(line);
    if (!line[0]) continue;
    char id[96] = {0};
    if (sscanf(line, "%d %95s", &recNum, id) == 2 && recNum > 0 && id[0]) {
      if (jobId && jobIdLen) strlcpy(jobId, id, jobIdLen);
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

static bool uploadReadPendingJobForRec(int wantedRec, char *jobId, size_t jobIdLen) {
  if (jobId && jobIdLen) jobId[0] = 0;
  if (wantedRec <= 0) return false;
  File f = SD.open(UPLOAD_PENDING_PATH, FILE_READ);
  if (!f) return false;
  char line[140];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    strTrim(line);
    if (!line[0]) continue;
    int recNum = 0;
    char id[96] = {0};
    if (sscanf(line, "%d %95s", &recNum, id) == 2 && recNum == wantedRec && id[0]) {
      if (jobId && jobIdLen) strlcpy(jobId, id, jobIdLen);
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

static bool uploadPollPendingRecMounted(int recNum, const char *jobId, bool allowConnect) {
  if (recNum <= 0 || !jobId || !jobId[0]) return false;
  if (!loadUploadConfig()) { g_uploadStatus = UPSTAT_NO_CFG; return false; }
  if (!uploadCfg.syncEnabled) { wifiPowerDown(); g_uploadStatus = UPSTAT_SYNC_OFF; return false; }
  if (!allowConnect && WiFi.status() != WL_CONNECTED) return false;
  if (!ensureWifiConnected()) {
    g_uploadStatus = g_uploadPausedForInput ? UPSTAT_PROCESSING : UPSTAT_WIFI_ERR;
    return false;
  }
  char host[65], urlPath[96], hostHeader[72];
  uint16_t port = 80;
  if (!parseHttpUrl(uploadCfg.url, host, sizeof(host), port, urlPath, sizeof(urlPath))) {
    g_uploadStatus = UPSTAT_BAD_URL;
    wifiPowerDown();
    return false;
  }
  if (port == 80) snprintf(hostHeader, sizeof(hostHeader), "%s", host);
  else snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);
  uint8_t status = uploadCheckJobStatusMounted(host, port, hostHeader, urlPath, jobId);
  g_uploadStatus = status;
  if (status == UPSTAT_DONE) {
    uploadMarkDone(recNum);
    uploadRemoveLineByRecMounted(UPLOAD_PENDING_PATH, recNum, uploadPendingBits);
    return true;
  }
  if (status == UPSTAT_MODEL_ERR || status == UPSTAT_JOB_ERR) {
    uploadMarkError(recNum, status == UPSTAT_MODEL_ERR);
    return true;
  }
  return false;
}

static bool uploadPollPendingMounted(bool allowConnect) {
  int recNum = 0;
  char jobId[96];
  if (!uploadReadFirstPending(recNum, jobId, sizeof(jobId))) return false;
  return uploadPollPendingRecMounted(recNum, jobId, allowConnect);
}

static uint8_t validateUploadConfigMounted() {
  if (!loadUploadConfig()) return UPSTAT_NO_CFG;
  if (!uploadCfg.syncEnabled) return UPSTAT_SYNC_OFF;
  char host[65], urlPath[96];
  uint16_t port = 80;
  if (!parseHttpUrl(uploadCfg.url, host, sizeof(host), port, urlPath, sizeof(urlPath))) return UPSTAT_BAD_URL;
  return UPSTAT_IDLE;
}

static bool uploadOneJobMounted() {
  g_uploadPausedForInput = false;
  if (!loadUploadConfig()) { g_uploadStatus = UPSTAT_NO_CFG; return false; }
  if (!uploadCfg.syncEnabled) { wifiPowerDown(); g_uploadStatus = UPSTAT_SYNC_OFF; return false; }
  auto failAfterWifi = [&](uint8_t status) {
    g_uploadStatus = status;
    wifiPowerDown();
    return false;
  };
  int recNum = 0;
  uint8_t kind = REC_NORMAL;
  if (!uploadReadFirstJob(recNum, kind)) return false;
  g_uploadActiveRec = recNum;
  g_uploadStatus = UPSTAT_UPLOADING;
  char path[40];
  uint8_t currentKind = recKindOf(recNum);
  recordingPathKind(recNum, currentKind, path, sizeof(path));
  if (!SD.exists(path) && currentKind != kind) recordingPathKind(recNum, kind, path, sizeof(path));
  if (!SD.exists(path)) {
    uploadDropFirstJob();
    g_uploadStatus = UPSTAT_NO_FILE;
    return true;
  }
  if (!ensureWifiConnected()) {
    if (g_uploadCancelRequested) {
      uploadDropFirstJob();
      g_uploadCancelRequested = false;
      g_uploadStatus = UPSTAT_ABORTED;
      wifiPowerDown();
      return true;
    }
    g_uploadStatus = g_uploadPausedForInput ? UPSTAT_QUEUED : UPSTAT_WIFI_ERR;
    return false;
  }
  char recordedAt[24] = {0};
  if (!boxReadRecordedAtMounted(recNum, recordedAt, sizeof(recordedAt))) boxFormatNow(recordedAt, sizeof(recordedAt));
  File f = SD.open(path, FILE_READ);
  if (!f || f.size() <= 44) { if (f) f.close(); uploadDropFirstJob(); g_uploadStatus = UPSTAT_NO_FILE; wifiPowerDown(); return true; }
  uint32_t originalSize = f.size();
  uint32_t pcmBytes = originalSize - 44;
  uint32_t sampleCount = pcmBytes / 2;
  bool useAdpcm = originalSize > UPLOAD_ADPCM_THRESHOLD_BYTES && sampleCount > 1;
  int16_t adpcmPredictor = 0;
  uint8_t adpcmIndex = 0;
  uint32_t bodyBytes = originalSize;
  if (useAdpcm) {
    uint8_t first[2] = {0, 0};
    f.seek(44);
    if (f.read(first, 2) != 2) { f.close(); return failAfterWifi(UPSTAT_NO_FILE); }
    adpcmPredictor = readLe16(first);
    bodyBytes = sampleCount / 2;  // ceil((sampleCount - 1) / 2)
  }
  char host[65], urlPath[96];
  uint16_t port = 80;
  if (!parseHttpUrl(uploadCfg.url, host, sizeof(host), port, urlPath, sizeof(urlPath))) { f.close(); return failAfterWifi(UPSTAT_BAD_URL); }
  char name[18];
  char hostHeader[72];
  snprintf(name, sizeof(name), "REC_%04d.wav", recNum);
  if (port == 80) snprintf(hostHeader, sizeof(hostHeader), "%s", host);
  else snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);
  WiFiClient client;
  client.setTimeout(4000);
  if (!client.connect(host, port, 3000)) { f.close(); return failAfterWifi(UPSTAT_HTTP_ERR); }
  client.setNoDelay(true);
  auto abortUploadNow = [&]() {
    client.stop();
    f.close();
    if (g_uploadCancelRequested) {
      uploadDropFirstJob();
      g_uploadCancelRequested = false;
      return failAfterWifi(UPSTAT_ABORTED);
    }
    return failAfterWifi(UPSTAT_QUEUED);
  };
  client.printf("POST %s HTTP/1.1\r\n", urlPath);
  client.printf("Host: %s\r\n", hostHeader);
  client.print("Connection: close\r\n");
  client.print(useAdpcm ? "Content-Type: application/x-cardputer-adpcm\r\n" : "Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)bodyBytes);
  if (useAdpcm) {
    client.print("X-Audio-Encoding: ima-adpcm\r\n");
    client.printf("X-Original-Content-Length: %u\r\n", (unsigned)originalSize);
    client.printf("X-Adpcm-Pcm-Bytes: %u\r\n", (unsigned)pcmBytes);
    client.printf("X-Adpcm-Sample-Rate: %u\r\n", (unsigned)REC_RATE);
    client.printf("X-Adpcm-Initial-Predictor: %d\r\n", (int)adpcmPredictor);
    client.printf("X-Adpcm-Initial-Index: %u\r\n", (unsigned)adpcmIndex);
  }
  client.printf("X-Upload-Token: %s\r\n", uploadCfg.token);
  client.printf("X-Device-Id: %s\r\n", uploadCfg.device);
  client.printf("X-Wifi-Rssi: %d\r\n", WiFi.RSSI());
  IPAddress localIp = WiFi.localIP();
  client.printf("X-Wifi-IP: %u.%u.%u.%u\r\n", localIp[0], localIp[1], localIp[2], localIp[3]);
  if (recordedAt[0]) client.printf("X-Recorded-At: %s\r\n", recordedAt);
  client.printf("X-Recording-Name: %s\r\n\r\n", name);
  static uint8_t buf[UPLOAD_CHUNK_BYTES];
  static uint8_t adpcmBuf[UPLOAD_CHUNK_BYTES / 2 + 1];
  uint8_t uiTick = 0;
  if (useAdpcm) {
    f.seek(46);
    bool haveNibble = false;
    uint8_t packed = 0;
    while (f.available()) {
      size_t n = f.read(buf, sizeof(buf));
      if (n == 0) break;
      n &= ~((size_t)1);
      size_t outN = 0;
      for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t nibble = imaAdpcmEncodeNibble(readLe16(buf + i), adpcmPredictor, adpcmIndex);
        if (!haveNibble) {
          packed = nibble;
          haveNibble = true;
        } else {
          adpcmBuf[outN++] = packed | (nibble << 4);
          haveNibble = false;
        }
      }
      if (outN && client.write(adpcmBuf, outN) != outN) { client.stop(); f.close(); return failAfterWifi(UPSTAT_HTTP_ERR); }
      if ((++uiTick & 0x03) == 0) {
        M5Cardputer.update();
        if (keyUploadAbort()) return abortUploadNow();
        delay(0);
      }
    }
    if (haveNibble && client.write(&packed, 1) != 1) { client.stop(); f.close(); return failAfterWifi(UPSTAT_HTTP_ERR); }
  } else {
    while (f.available()) {
      size_t n = f.read(buf, sizeof(buf));
      if (n == 0) break;
      if (client.write(buf, n) != n) { client.stop(); f.close(); return failAfterWifi(UPSTAT_HTTP_ERR); }
      if ((++uiTick & 0x07) == 0) {
        M5Cardputer.update();
        if (keyUploadAbort()) return abortUploadNow();
        delay(0);
      }
    }
  }
  int code = readHttpStatusCodeOnly(client, 5000);
  client.stop();
  f.close();
  if ((code >= 200 && code < 300) || code == 409) {
    uploadMarkDone(recNum);
    uploadDropFirstJob();
    int nextRec = 0;
    uint8_t nextKind = REC_NORMAL;
    if (!uploadReadFirstJob(nextRec, nextKind)) wifiPowerDown();
    g_uploadStatus = UPSTAT_DONE;
    return true;
  }
  g_uploadStatus = UPSTAT_HTTP_ERR;
  wifiPowerDown();
  return false;
}
#else
static void boxSaveRecordedAtMounted(int recNum) {
  (void)recNum;
}

static bool uploadOneJobMounted() {
  return false;
}

static uint8_t validateUploadConfigMounted() {
  return UPSTAT_IDLE;
}
#endif

static const char *uploadStatusLabel(uint8_t status) {
  switch (status) {
    case UPSTAT_UPLOADING: return "GO";
    case UPSTAT_QUEUED: return "WT";
    case UPSTAT_DONE: return "OKK";
    case UPSTAT_NO_CFG: return "NO CFG";
    case UPSTAT_BAD_URL: return "BAD URL";
    case UPSTAT_WIFI_ERR: return "WIFI ERR";
    case UPSTAT_HTTP_ERR: return "HTTP ERR";
    case UPSTAT_NO_SD: return "NO SD";
    case UPSTAT_NO_FILE: return "NO FILE";
    case UPSTAT_ABORTED: return "ABORT";
    case UPSTAT_SYNC_OFF: return "SYNC OFF";
    case UPSTAT_PROCESSING: return "OKK";
    case UPSTAT_MODEL_ERR: return "OKK";
    case UPSTAT_JOB_ERR: return "OKK";
    default: return "WT";
  }
}

static bool uploadQueuedJobsMounted(uint8_t maxJobs = 1, uint32_t budgetMs = 9000) {
  bool changed = false;
  uint32_t start = millis();
  for (uint8_t i = 0; i < maxJobs; i++) {
    if (millis() - start > budgetMs) break;
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      g_uploadPausedForInput = true;
      g_uploadStatus = UPSTAT_QUEUED;
      break;
    }
    bool oneChanged = uploadOneJobMounted();
    if (!oneChanged) break;
    changed = true;
  }
#if UPLOAD_WIFI_ENABLED
  int nextRec = 0;
  uint8_t nextKind = REC_NORMAL;
  if (uploadReadFirstJob(nextRec, nextKind)) wifiPowerDown();
#endif
  return changed;
}

static bool uploadQueueHasJobMounted(int *recOut = nullptr) {
  int recNum = 0;
  uint8_t kind = REC_NORMAL;
  bool hasJob = uploadReadFirstJob(recNum, kind);
  if (hasJob && recOut) *recOut = recNum;
  return hasJob;
}

static void drawUploadMode(const char *status, uint16_t col) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("UPLOAD");
  FONT_TIMER(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(28, 50);
  d.print(status);
  drawFooter("ESC ABORT");
}

static bool runUploadBatchMounted(bool visible) {
#if !UPLOAD_WIFI_ENABLED
  (void)visible;
  return false;
#else
  if (!sdMount()) {
    g_uploadStatus = UPSTAT_NO_SD;
    if (visible) drawUploadMode(uploadStatusLabel(g_uploadStatus), COL_RED);
    return false;
  }
  int firstRec = 0;
  if (!uploadQueueHasJobMounted(&firstRec)) {
    g_uploadActiveAnnounced = false;
    g_uploadActiveRec = 0;
    g_uploadStatus = UPSTAT_IDLE;
    SD.end();
    if (visible) drawUploadMode("NO WT", COL_DIM);
    return false;
  }
  g_uploadActiveAnnounced = visible;
  g_uploadActiveRec = firstRec;
  g_uploadStatus = UPSTAT_UPLOADING;
  if (visible) drawUploadMode("GO", COL_GREEN);
  bool changed = uploadQueuedJobsMounted(UPLOAD_BATCH_MAX_JOBS, UPLOAD_BATCH_BUDGET_MS);
  int nextRec = 0;
  bool hasNext = uploadQueueHasJobMounted(&nextRec);
  g_uploadActiveAnnounced = false;
  g_uploadActiveRec = hasNext ? nextRec : 0;
  if (hasNext) {
    g_uploadStatus = UPSTAT_QUEUED;
    if (visible) drawUploadMode("WT", COL_GREEN);
  } else if (changed && g_uploadStatus == UPSTAT_UPLOADING) {
    g_uploadStatus = UPSTAT_DONE;
    if (visible) drawUploadMode("OKK", COL_GREEN);
  } else if (visible) {
    drawUploadMode(uploadStatusLabel(g_uploadStatus), (g_uploadStatus == UPSTAT_DONE || g_uploadStatus == UPSTAT_QUEUED) ? COL_GREEN : COL_RED);
  }
  SD.end();
  return changed;
#endif
}

static bool uploadModeHoldTriggered() {
  uint32_t start = millis();
  while (keyUpload() && !keyTab()) {
    M5Cardputer.update();
    if (millis() - start >= UPLOAD_MODE_HOLD_MS) return true;
    delay(10);
  }
  return false;
}

static bool systemIdleTick() {
  return false;
}

// ---------- 按键摩擦后处理: 只压低低能量、高变化率的细碎刮擦声 ----------
static bool suppressKeyFriction(const char *path, uint32_t dataBytes, bool abortOnKey) {
  if (dataBytes < REC_N * 2) return false;
  char tmp[48];
  snprintf(tmp, sizeof(tmp), "%s.s", path);

  File in = SD.open(path, FILE_READ);
  if (!in || in.size() <= 44) { if (in) in.close(); return false; }
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { in.close(); return false; }

  writeWavHeader(out, REC_RATE, 0);
  in.seek(44);

  static int16_t scratchBuf[REC_N];
  uint32_t remaining = dataBytes;
  uint32_t outBytes = 0;
  int hold = 0;
  int32_t lp = 0;

  while (remaining > 0) {
    size_t want = remaining < sizeof(scratchBuf) ? remaining : sizeof(scratchBuf);
    int rd = in.read((uint8_t *)scratchBuf, want);
    if (rd <= 0) break;
    int got = rd / 2;

    int64_t sumSq = 0;
    int64_t sumDiff = 0;
    for (int i = 0; i < got; i++) {
      int32_t s = scratchBuf[i];
      sumSq += (int64_t)s * s;
      if (i > 0) sumDiff += abs((int)(scratchBuf[i] - scratchBuf[i - 1]));
    }
    int32_t rms = (int32_t)sqrtf((float)((double)sumSq / got));
    int32_t avgDiff = (got > 1) ? (int32_t)(sumDiff / (got - 1)) : 0;
    bool scratch = ((rms < 1900 && avgDiff > 140 && ((int64_t)avgDiff * 256 > (int64_t)rms * 90)) ||
                    (rms < 2200 && avgDiff > 300));
    if (scratch) hold = 2;

    if (scratch || hold > 0) {
      for (int i = 0; i < got; i++) {
        lp += (int32_t)(((int64_t)(scratchBuf[i] - lp) * 96) >> 8);
        int32_t hi = (int32_t)scratchBuf[i] - lp;
        int32_t y = lp + ((hi * 64) >> 8);
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        scratchBuf[i] = (int16_t)y;
      }
      if (hold > 0) hold--;
    } else if (got > 0) {
      lp = scratchBuf[got - 1];
    }

    out.write((uint8_t *)scratchBuf, got * 2);
    outBytes += got * 2;
    remaining -= got * 2;
    if (abortOnKey) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isPressed()) {
        out.close();
        in.close();
        SD.remove(tmp);
        return false;
      }
    }
  }

  writeWavHeader(out, REC_RATE, outBytes);
  out.flush();
  out.close();
  in.close();
  if (outBytes == 0) { SD.remove(tmp); return false; }
  return replaceFileWithTemp(path, tmp);
}

static bool cleanFrictionForRec(int recNum, bool abortOnKey) {
  if (recNum <= 0) return false;
  if (frictionDone(recNum)) return true;
  if (!sdMount()) return false;
  char p[40];
  recordingPathForRec(recNum, p, sizeof(p));
  File f = SD.open(p, FILE_READ);
  if (!f || f.size() <= 44) { if (f) f.close(); SD.end(); return false; }
  uint32_t dataBytes = f.size() - 44;
  setRecDuration(recNum, f.size());
  f.close();
  bool ok = suppressKeyFriction(p, dataBytes, abortOnKey);
  SD.end();
  if (ok) {
    setFrictionDone(recNum, true);
    setFrictionPending(recNum, false);
  }
  return ok;
}

static bool processPendingFrictionQueue(bool &abortedByKey) {
  abortedByKey = false;
  bool didWork = false;
  for (int i = recCount - 1; i >= 0; i--) {
    int recNum = recList[i];
    uint16_t dur = getRecDuration(recNum);
    if (!frictionPending(recNum) || dur <= FRICTION_NOW_SEC || dur > FRICTION_IDLE_MAX_SEC) continue;
    didWork = true;
    cleanFrictionForRec(recNum, true);
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isPressed()) {
      abortedByKey = true;
      break;
    }
  }
  return didWork;
}

// ---------- 降噪: FFT 谱减法(后处理) ----------
static void nr_fft(float *re, float *im, int n, bool inv) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = 2.0f * PI / len * (inv ? 1.0f : -1.0f);
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cwr = 1, cwi = 0;
      for (int k = 0; k < len / 2; k++) {
        float vr = re[i + k + len / 2] * cwr - im[i + k + len / 2] * cwi;
        float vi = re[i + k + len / 2] * cwi + im[i + k + len / 2] * cwr;
        float ur = re[i + k], ui = im[i + k];
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        float ncwr = cwr * wr - cwi * wi; cwi = cwr * wi + cwi * wr; cwr = ncwr;
      }
    }
  }
  if (inv) { for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; } }
}

void noiseReduce(const char *path) {
  auto &d = M5Cardputer.Display;
  const int N = 256, HOP = 128;
  static float win[256], noiseMag[129], re[256], im[256], frame[256], ola[256];
  static int16_t hopbuf[128];

  d.fillScreen(COL_BG);
  drawHeader("降噪处理中...");

  if (!sdMount()) { showMsg("降噪", "SD 读取失败", COL_RED); return; }
  File in = SD.open(path, FILE_READ);
  if (!in || in.size() <= 44) { if (in) in.close(); SD.end(); showMsg("降噪", "文件无效", COL_RED); return; }
  uint32_t dataBytes = in.size() - 44;

  for (int i = 0; i < N; i++) win[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / N));
  for (int i = 0; i < 129; i++) noiseMag[i] = 1e30f;
  for (int i = 0; i < N; i++) { frame[i] = 0; ola[i] = 0; }

  in.seek(44);
  size_t rd;
  while ((rd = in.read((uint8_t *)hopbuf, HOP * 2)) > 0) {
    int got = rd / 2;
    for (int i = 0; i < N - HOP; i++) frame[i] = frame[i + HOP];
    for (int i = 0; i < HOP; i++) frame[N - HOP + i] = (i < got) ? (float)hopbuf[i] : 0.0f;
    for (int i = 0; i < N; i++) { re[i] = frame[i] * win[i]; im[i] = 0; }
    nr_fft(re, im, N, false);
    for (int b = 0; b <= N / 2; b++) { float m = sqrtf(re[b] * re[b] + im[b] * im[b]); if (m < noiseMag[b]) noiseMag[b] = m; }
  }

  char tmp[48]; snprintf(tmp, sizeof(tmp), "%s.t", path);
  SD.remove(tmp);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { in.close(); SD.end(); showMsg("降噪", "无法写临时文件", COL_RED); return; }
  writeWavHeader(out, REC_RATE, 0);
  for (int i = 0; i < N; i++) { frame[i] = 0; ola[i] = 0; }
  in.seek(44);
  uint32_t outBytes = 0, processed = 0, lastP = 0;
  const float ALPHA = 3.0f, BETA = 0.05f;
  while ((rd = in.read((uint8_t *)hopbuf, HOP * 2)) > 0) {
    int got = rd / 2;
    for (int i = 0; i < N - HOP; i++) frame[i] = frame[i + HOP];
    for (int i = 0; i < HOP; i++) frame[N - HOP + i] = (i < got) ? (float)hopbuf[i] : 0.0f;
    for (int i = 0; i < N; i++) { re[i] = frame[i] * win[i]; im[i] = 0; }
    nr_fft(re, im, N, false);
    for (int b = 0; b <= N / 2; b++) {
      float mag = sqrtf(re[b] * re[b] + im[b] * im[b]);
      float clean = mag - ALPHA * noiseMag[b];
      float fl = BETA * mag;
      if (clean < fl) clean = fl;
      float sc = (mag > 1e-3f) ? (clean / mag) : 0.0f;
      re[b] *= sc; im[b] *= sc;
      if (b > 0 && b < N / 2) { re[N - b] = re[b]; im[N - b] = -im[b]; }
    }
    nr_fft(re, im, N, true);
    for (int i = 0; i < N; i++) ola[i] += re[i];
    for (int i = 0; i < HOP; i++) {
      float v = ola[i];
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      hopbuf[i] = (int16_t)v;
    }
    out.write((uint8_t *)hopbuf, HOP * 2);
    outBytes += HOP * 2;
    for (int i = 0; i < N - HOP; i++) ola[i] = ola[i + HOP];
    for (int i = N - HOP; i < N; i++) ola[i] = 0;
    processed += rd;
    if (processed - lastP > 16384) {
      lastP = processed;
      d.fillRect(0, 36, d.width(), 22, COL_BG);
      d.setTextColor(COL_GREEN, COL_BG);
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)((uint64_t)processed * 100 / dataBytes));
      drawDseg14Text(d, 8, 38, pctBuf, COL_GREEN);
    }
  }
  writeWavHeader(out, REC_RATE, outBytes);
  out.flush(); out.close();
  in.close();
  replaceFileWithTemp(path, tmp);
  SD.end();
}

// ---------- 录音画面 (canvas: CONTENT_W × 135, 推送到 x=0) ----------
// A线: waveBars[] 小柱轨道(上半区)  B线: wave[]实时示波器(下半区)
void drawRecCanvas(M5Canvas &cv, uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);

  cv.fillScreen(COL_BG);

  // ── 顶栏 ────────────────────────────────────────────────────────
  const char *status = ready ? "READY" : (paused ? "PAUSE" : "REC");
  uint16_t statusCol = (ready || paused) ? COL_DIM : COL_GREEN;
  drawStatusTitleNoLine(cv, status, statusCol);
  if (!ready && !paused) {
    if (blink) cv.fillCircle(48, 9, 5, COL_RED);
    else cv.drawCircle(48, 9, 5, 0x2000);
  }
  // 电量 (亮绿, 黑底确保可读)

  // ── A线: 滚动历史 (上半区, 中轴 y=39) ──────────────────────────
  cv.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(cv);

  // ── B线: 实时示波器 (下半区, 中轴 y=85) ─────────────────────────
  cv.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(cv, recLiveWave, B_CY);

  // ── 计时器 (底部左侧) ───────────────────────────────────────────
  uint32_t s = elapsedMs / 1000;
  cv.setTextColor(COL_GREEN, COL_BG);
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(cv, 4, WAVE_BOT + 2, recTime, COL_GREEN);
  drawDeleteProgress(cv, deleteHeldMs);
}

void drawRecCanvas(m5gfx::M5GFX &g, uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);

  g.fillScreen(COL_BG);

  const char *status = ready ? "READY" : (paused ? "PAUSE" : "REC");
  uint16_t statusCol = (ready || paused) ? COL_DIM : COL_GREEN;
  drawStatusTitleNoLine(g, status, statusCol);
  if (!ready && !paused) {
    if (blink) g.fillCircle(48, 9, 5, COL_RED);
    else g.drawCircle(48, 9, 5, 0x2000);
  }

  g.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(g);

  g.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(g, recLiveWave, B_CY);

  uint32_t s = elapsedMs / 1000;
  g.setTextColor(COL_GREEN, COL_BG);
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, recTime, COL_GREEN);
  drawDeleteProgress(g, deleteHeldMs);
}

static void drawRecWaveCanvas(M5Canvas &cv, int16_t *wave) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);
  cv.fillScreen(COL_BG);
  cv.drawFastHLine(0, A_CY - WAVE_TOP, CONTENT_W, 0x0440);
  drawTrackBarsLocal(cv);
  cv.drawFastHLine(0, B_CY - WAVE_TOP, CONTENT_W, 0x0440);
  drawLiveWaveVisual(cv, recLiveWave, B_CY - WAVE_TOP);
}

static void drawRecWaveDirect(m5gfx::M5GFX &g, int16_t *wave) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(g, recLiveWave, B_CY);
}

static void drawRecChrome(m5gfx::M5GFX &g, uint32_t elapsedMs, bool blink, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  if (ready || paused) {
    const char *status = ready ? "READY" : "PAUSE";
    uint16_t statusCol = COL_DIM;
    drawStatusTitleNoLine(g, status, statusCol);
  } else {
    g.fillRect(42, 3, 14, 14, COL_BG);
    if (blink) g.fillCircle(48, 9, 5, COL_RED);
    else g.drawCircle(48, 9, 5, 0x2000);
  }

  g.fillRect(0, WAVE_BOT + 1, 84, 135 - WAVE_BOT - 1, COL_BG);
  uint32_t s = elapsedMs / 1000;
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, recTime, COL_GREEN);
  drawDeleteProgress(g, deleteHeldMs);
}

// 录制. 开机自动进入; 调用此函数后创建文件并开始写入
int recordingScreen() {
  pauseUploadForMedia();
  g_mediaBusy = true;
  auto &d = M5Cardputer.Display;
  g_afterRecord = R_LIST;
  g_uploadAfterRecord = false;

  // 准备阶段仍显示 READY, 真正开始采样后才亮 REC 红点
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(recLiveWave, 0, sizeof(recLiveWave));
  M5Canvas cv(&d);
  M5Canvas waveCv(&d);
  bool useWaveSprite = waveCv.createSprite(CONTENT_W, WAVE_H) != nullptr;
  M5Canvas bottomCv(&d);
  bool useBottomSprite = useWaveSprite && bottomCv.createSprite(CONTENT_W, CHROME_H) != nullptr;
  bool useSprite = !useWaveSprite && cv.createSprite(CONTENT_W, d.height()) != nullptr;
  const uint32_t recFrameMs = (useWaveSprite || useSprite) ? REC_UI_FRAME_MS : 90;
  uint32_t lastRecChromeSec = 0xFFFFFFFFUL;
  bool lastRecChromeBlink = false;
  uint32_t lastRecChromeHeldBucket = 0xFFFFFFFFUL;
  auto drawRecFrame = [&](uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool pausedFrame = false, uint32_t deleteHeldMs = 0) {
    if (useSprite) {
      drawRecCanvas(cv, elapsedMs, blink, wave, ready, pausedFrame, deleteHeldMs);
      cv.pushSprite(0, 0);
    } else {
      drawRecCanvas(d, elapsedMs, blink, wave, ready, pausedFrame, deleteHeldMs);
    }
    lastRecChromeSec = elapsedMs / 1000;
    lastRecChromeBlink = blink;
    lastRecChromeHeldBucket = deleteHeldMs / 60;
  };
  auto drawRecChromePartial = [&](uint32_t elapsedMs, bool blink, bool ready = false, bool pausedFrame = false, uint32_t deleteHeldMs = 0, bool forceTop = false, bool forceBottom = false) {
    if (!useWaveSprite) {
      if (useSprite) {
        drawRecFrame(elapsedMs, blink, nullptr, ready, pausedFrame, deleteHeldMs);
      } else {
        drawRecChrome(d, elapsedMs, blink, ready, pausedFrame, deleteHeldMs);
      }
      return;
    }
    uint32_t sec = elapsedMs / 1000;
    uint32_t heldBucket = deleteHeldMs / 60;
    if (forceTop || ready || pausedFrame) {
      const char *status = ready ? "READY" : (pausedFrame ? "PAUSE" : "REC");
      uint16_t statusCol = (ready || pausedFrame) ? COL_DIM : COL_GREEN;
      drawStatusTitleNoLine(d, status, statusCol);
      if (!ready && !pausedFrame) {
        d.fillRect(42, 3, 14, 14, COL_BG);
        if (blink) d.fillCircle(48, 9, 5, COL_RED);
        else d.drawCircle(48, 9, 5, 0x2000);
      }
    } else if (blink != lastRecChromeBlink) {
      d.fillRect(42, 3, 14, 14, COL_BG);
      if (blink) d.fillCircle(48, 9, 5, COL_RED);
      else d.drawCircle(48, 9, 5, 0x2000);
    }
    if (forceBottom || sec != lastRecChromeSec || heldBucket != lastRecChromeHeldBucket) {
      if (useBottomSprite) {
        drawRecBottomCanvas(bottomCv, elapsedMs, deleteHeldMs);
        bottomCv.pushSprite(0, CHROME_TOP);
      } else {
        d.fillRect(0, WAVE_BOT + 1, 84, 135 - WAVE_BOT - 1, COL_BG);
        uint32_t s = elapsedMs / 1000;
        char recTime[8];
        snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        drawDseg14Text(d, 4, WAVE_BOT + 2, recTime, COL_GREEN);
        drawDeleteProgress(d, deleteHeldMs);
      }
    }
    lastRecChromeSec = sec;
    lastRecChromeBlink = blink;
    lastRecChromeHeldBucket = heldBucket;
  };
  auto drawRecRealtime = [&](uint32_t elapsedMs, bool blink, int16_t *wave, uint32_t deleteHeldMs = 0) {
    if (!useWaveSprite) {
      if (useSprite) {
        drawRecFrame(elapsedMs, blink, wave, false, false, deleteHeldMs);
      } else {
        drawRecWaveDirect(d, wave);
        uint32_t sec = elapsedMs / 1000;
        uint32_t heldBucket = deleteHeldMs / 60;
        if (sec != lastRecChromeSec || blink != lastRecChromeBlink || heldBucket != lastRecChromeHeldBucket) {
          drawRecChrome(d, elapsedMs, blink, false, false, deleteHeldMs);
          lastRecChromeSec = sec;
          lastRecChromeBlink = blink;
          lastRecChromeHeldBucket = heldBucket;
        }
      }
      return;
    }
    drawRecWaveCanvas(waveCv, wave);
    waveCv.pushSprite(0, WAVE_TOP);
    uint32_t sec = elapsedMs / 1000;
    uint32_t heldBucket = deleteHeldMs / 60;
    if (sec != lastRecChromeSec || blink != lastRecChromeBlink || heldBucket != lastRecChromeHeldBucket) {
      drawRecChromePartial(elapsedMs, blink, false, false, deleteHeldMs);
    }
  };
  auto drawRecToast = [&](const char *msg, uint16_t col = COL_GREEN) {
    if (useSprite) drawCanvasToast(cv, msg, col);
    else drawActionToast(msg, col);
  };
  auto drawRecVolume = [&](uint16_t col = COL_GREEN) {
    if (useSprite) drawCanvasVolumeToast(cv, col);
    else drawVolumeToast(col);
  };
  auto drawRecBrightness = [&](uint16_t col = COL_GREEN) {
    if (useSprite) drawCanvasBrightnessToast(cv, col);
    else drawBrightnessToast(col);
  };
  auto drawRecSave = [&]() {
    if (useSprite) drawCanvasSaveBadge(cv);
    else drawActionToast("SAVE", COL_GREEN);
  };
  auto cleanupRecSprite = [&]() {
    if (useBottomSprite) bottomCv.deleteSprite();
    if (useWaveSprite) waveCv.deleteSprite();
    if (useSprite) cv.deleteSprite();
    g_mediaBusy = false;
  };
  drawRecFrame(0, false, nullptr, true);

  if (!sdMount()) { cleanupRecSprite(); showMsg("录音机", "未检测到 SD 卡", COL_RED); return 0; }
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  int idx = nextRecordingIndex();
  uploadForgetRecordMounted(idx);
  char path[40];
  recordingPathKind(idx, REC_NORMAL, path, sizeof(path));
  File f = SD.open(path, FILE_WRITE);
  if (!f) { cleanupRecSprite(); SD.end(); showMsg("录音机", "无法写入文件", COL_RED); return 0; }
  nextRecHint = (idx < 9999) ? idx + 1 : 9999;
  writeWavHeader(f, REC_RATE, 0);

  bool mustRearm = forceMicRearm;
  bool micWasReady = micInputReady && !mustRearm;
  if (!prepareMicInput(mustRearm)) { cleanupRecSprite(); f.close(); SD.end(); showMsg("录音机", "麦克风启动失败", COL_RED); return 0; }
  if (!micWasReady && !mustRearm) delay(20);
  if (mustRearm) {
    for (int i = 0; i < REC_REARM_SETTLE_BUFFERS; i++) {
      M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
      while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    }
    forceMicRearm = false;
  }

  uint32_t dataBytes = 0;
  size_t recWriteSamples = 0;
  int b = 0;
  int lastSavedIdx = 0;
  bool currentFileOpen = true;
  bool autoSegmented = false;
  uint32_t startMs = millis(), pausedTotal = 0, pauseStart = 0, lastDraw = 0;
  bool paused = false;
  auto activeElapsed = [&]() -> uint32_t {
    uint32_t now = millis();
    uint32_t currentPause = paused ? (now - pauseStart) : 0;
    return now - startMs - pausedTotal - currentPause;
  };
  auto queueRecordBuffers = [&]() {
    b = 0;
    M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
    M5Cardputer.Mic.record(recBuf[1], REC_N, REC_RATE);
  };
  auto flushRecWrite = [&]() {
    if (recWriteSamples == 0) return;
    size_t bytes = recWriteSamples * sizeof(int16_t);
    f.write((uint8_t *)recWriteBuf, bytes);
    dataBytes += bytes;
    recWriteSamples = 0;
  };
  auto finishCurrentSegment = [&](bool autoSplit, bool cancelCurrent) {
    if (!currentFileOpen) return false;
    flushRecWrite();
    uint32_t finalData = dataBytes;
    if (!autoSplit) {
      uint32_t tailTrim = REC_RATE * 6 / 10;
      finalData = (dataBytes > tailTrim) ? (dataBytes - tailTrim) : 0;
    }
    bool drop = cancelCurrent || finalData < REC_RATE;
    if (!drop) {
      writeWavHeader(f, REC_RATE, finalData);
      f.close();
      currentFileOpen = false;
      setRecDuration(idx, 44 + finalData);
      appendRecordingOrderMounted(idx);
      boxSaveRecordedAtMounted(idx);
      bool queueThis = autoSplit || autoSegmented || g_uploadAfterRecord;
      if (queueThis && enqueueUploadMounted(idx, REC_NORMAL)) {
        lastUploadTickMs = millis();
      }
      if (autoSplit) autoSegmented = true;
      if (!autoSplit) {
        uint16_t dur = getRecDuration(idx);
        if (dur <= FRICTION_NOW_SEC) {
          if (suppressKeyFriction(path, finalData)) setFrictionDone(idx, true);
        } else if (dur <= FRICTION_IDLE_MAX_SEC) {
          setFrictionPending(idx, true);
        }
      }
      insertRecListAtEnd(idx);
      lastSavedIdx = idx;
      nextRecHint = (idx < 9999) ? idx + 1 : 9999;
      writeNextIndexCache(nextRecHint);
      return true;
    }
    f.close();
    currentFileOpen = false;
    SD.remove(path);
    nextRecHint = idx;
    writeNextIndexCache(idx);
    return false;
  };
  auto openNextSegment = [&]() {
    idx = nextRecordingIndex();
    uploadForgetRecordMounted(idx);
    recordingPathKind(idx, REC_NORMAL, path, sizeof(path));
    f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    currentFileOpen = true;
    nextRecHint = (idx < 9999) ? idx + 1 : 9999;
    writeWavHeader(f, REC_RATE, 0);
    dataBytes = 0;
    recWriteSamples = 0;
    startMs = millis();
    pausedTotal = 0;
    pauseStart = 0;
    lastDraw = 0;
    return true;
  };
  queueRecordBuffers();
  drawRecChromePartial(0, true, false, false, 0, true, true);
  bool stop = false;
  bool cancelRec = false;
  bool recScreenOff = false;
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;
  bool ignoreStartKey = M5Cardputer.Keyboard.isPressed();
  int32_t recDc = 0;
  int32_t recHum = 0;
  int32_t recLpf = 0;
  int32_t recNoiseRms = 90;
  int32_t recSoftGateQ8 = 256;
  uint32_t skipBuffers = micWasReady ? 4 : 8;   // 准备阶段已热机, 只短掐头防按键声

  while (!stop) {
    while (true) {
      M5Cardputer.update();
      if (ignoreStartKey) {
        if (!M5Cardputer.Keyboard.isPressed()) ignoreStartKey = false;
      } else if (recScreenOff) {
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
          if (keyTab() || keySpace() || keyDel() || keyEnter() || keyUpload()) {
            recScreenOff = false;
            ignoreStartKey = true;
            delHoldStart = 0;
            lastDelDraw = 0;
            applyBrightness();
            drawRecChromePartial(activeElapsed(), true, false, paused, 0, true, true);
          }
        }
      } else if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyTab() && !keyUpload()) {
          recScreenOff = true;
          delHoldStart = 0;
          lastDelDraw = 0;
          M5Cardputer.Display.setBrightness(0);
        }
        else if (keySpace()) {
          if (paused) {
            paused = false;
            pausedTotal += millis() - pauseStart;
            if (skipBuffers < 2) skipBuffers = 2;
            queueRecordBuffers();
            drawRecChromePartial(activeElapsed(), true, false, false, 0, true, true);
          } else {
            paused = true;
            pauseStart = millis();
            while (M5Cardputer.Mic.isRecording() > 0) { M5Cardputer.update(); delay(1); }
            drawRecChromePartial(activeElapsed(), false, false, true, 0, true, true);
          }
          waitRelease();
        }
        else if (keyEsc()) { g_afterRecord = R_BACK; stop = true; break; }
        else if (keyTab() && keyUpload()) {
          bool cancelled = uploadCancelMounted();
          g_uploadStatus = cancelled ? UPSTAT_ABORTED : UPSTAT_IDLE;
          drawRecToast(cancelled ? "ABORT" : "NO WT", cancelled ? COL_GREEN : COL_DIM);
          waitRelease();
        }
        else if (keyUpload()) { drawRecToast("WT", COL_GREEN); g_uploadAfterRecord = true; g_afterRecord = R_LIST; stop = true; break; }
        else if (keyEnter()) { drawRecSave(); g_afterRecord = R_LIST; stop = true; break; }
        else if (keyAlt()) { drawRecToast("WAIT", COL_GREEN); g_afterRecord = R_NOISE; stop = true; break; }
        else if (keyVolUp()) { adjustPlayVolume(25); drawRecVolume(COL_GREEN); waitRelease(); }
        else if (keyVolDn()) { adjustPlayVolume(-25); drawRecVolume(COL_GREEN); waitRelease(); }
        else if (keyBrightUp()) { adjustBrightness(1); drawRecBrightness(COL_GREEN); waitRelease(); }
        else if (keyBrightDn()) { adjustBrightness(-1); drawRecBrightness(COL_GREEN); waitRelease(); }
        else if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) { if (recGain < 200) recGain += 4; }
        else if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) { if (recGain > 2) recGain -= 4; }
      }

      if (!ignoreStartKey && !recScreenOff) {
        if (keyDel()) {
          if (delHoldStart == 0) { delHoldStart = millis(); lastDelDraw = 0; }
          uint32_t held = millis() - delHoldStart;
          if (held >= DELETE_HOLD_MS) { cancelRec = true; g_afterRecord = R_BACK; stop = true; break; }
          if (held >= DELETE_HINT_MS && millis() - lastDelDraw > 60) {
            lastDelDraw = millis();
            drawRecChromePartial(activeElapsed(), false, false, false, held, false, true);
          }
        } else if (delHoldStart != 0) {
          delHoldStart = 0;
          drawRecChromePartial(activeElapsed(), false, false, false, 0, false, true);
        }
      }
      if (stop) break;
      if (paused) { delay(8); continue; }
      if (M5Cardputer.Mic.isRecording() < 2) break;
      delay(1);
    }
    if (stop) break;
    if (paused) continue;
    int16_t *filled = recBuf[b];
    processMicBuffer(filled, REC_N, recDc, recHum, recLpf, recNoiseRms, recSoftGateQ8);
    memcpy(recVisualBuf, filled, REC_N * sizeof(int16_t));
    if (skipBuffers > 0) skipBuffers--;
    else {
      memcpy(recWriteBuf + recWriteSamples, filled, REC_N * sizeof(int16_t));
      recWriteSamples += REC_N;
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
    if (recWriteSamples >= REC_WRITE_BATCH * REC_N) flushRecWrite();
    uint32_t now = millis();
    if (now - lastDraw >= recFrameMs) {
      lastDraw = now;
      bool blink = ((now / 400) % 2) == 0;
      uint32_t elapsed = activeElapsed();
      pushTrackBar(calcTrackAmp(recVisualBuf, REC_N));
      uint32_t held = delHoldStart ? millis() - delHoldStart : 0;
      if (!recScreenOff) {
        drawRecRealtime(elapsed, blink, recVisualBuf, held);
      }
    }
    if (activeElapsed() >= REC_AUTO_SEGMENT_MS) {
      finishCurrentSegment(true, false);
      if (!openNextSegment()) {
        g_afterRecord = R_LIST;
        stop = true;
      } else if (!recScreenOff) {
        drawRecChromePartial(0, true, false, false, 0, true, true);
      }
    }
  }
  if (recScreenOff) applyBrightness();
  cleanupRecSprite();
  if (currentFileOpen) finishCurrentSegment(false, cancelRec);
  SD.end();
  while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  if (g_afterRecord != R_BACK) {
    micInputReady = false;
    M5Cardputer.Mic.end();
    const uint8_t ES = 0x18;
    M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
    M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  }
  if (cancelRec && lastSavedIdx <= 0) return 0;
  return lastSavedIdx;
}

// ---------- 录音列表: ;/.选, 回车放, Tab+键绑定, Alt降噪, 空格录音, Esc退出, 长按Del删除 ----------
// 返回 R_BACK(退出列表并息屏) 或 R_RECORD(去录音)
static void deleteRecording(int recNum) {
  if (!sdMount()) return;
  char p[40];
  recordingPathKind(recNum, REC_NORMAL, p, sizeof(p));
  SD.remove(p);
  recordingPathKind(recNum, REC_SHORTCUT, p, sizeof(p));
  SD.remove(p);
  recordingPathKind(recNum, REC_IMPORTANT, p, sizeof(p));
  SD.remove(p);
  uploadForgetRecordMounted(recNum);
  if (recVolumeDirty) saveRecVolumeStateMounted();
  removeRecordingOrderMounted(recNum);
  SD.end();
  setShortcutRec(recNum, false);
  setImportantRec(recNum, false);
  setFrictionDone(recNum, false);
  setFrictionPending(recNum, false);
  setUploadQueued(recNum, false);
  setUploadDone(recNum, false);
  if (recNum > 0 && recNum <= MAX_REC) recDurationSec[recNum - 1] = 0;
  bool hotkeyRemoved = false;
  for (int i = 0; i < hotkeyCount; i++) {
    if (hotkeys[i].idx == recNum) {
      for (int j = i; j < hotkeyCount - 1; j++) hotkeys[j] = hotkeys[j + 1];
      hotkeyCount--; i--;
      hotkeyRemoved = true;
    }
  }
  if (hotkeyRemoved) saveHotkeys();
}

static int deleteUnmarkedRecordings() {
  if (!sdMount()) return 0;
  int deleted = 0;
  File dir = SD.open(REC_DIR);
  if (dir && dir.isDirectory()) {
    File e;
    while ((e = dir.openNextFile())) {
      int n = parseRecordingNumber(e.name());
      e.close();
      if (n <= 0) continue;
      char p[40];
      recordingPathKind(n, REC_NORMAL, p, sizeof(p));
      if (SD.remove(p)) {
        uploadForgetRecordMounted(n);
        deleted++;
      }
    }
    dir.close();
  }
  SD.end();
  if (recVolumeDirty && sdMount()) {
    saveRecVolumeStateMounted();
    SD.end();
  }
  scanRecordings(true);
  return deleted;
}

static bool confirmDeleteUnmarked() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("清理录音");
  FONT_CN_16(d);
  d.setTextColor(COL_RED, COL_BG);
  d.setCursor(12, 38);
  d.print("删除所有普通录音?");
  FONT_CN_12(d);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(12, 66);
  d.print("保留 KEY / IMP");
  drawFooter("回车确认  退格取消");
  waitRelease();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (keyEnter()) { waitRelease(); return true; }
      if (keyDel() || keyEsc()) { waitRelease(); return false; }
    }
    delay(8);
  }
}

static bool confirmNoiseReduce(int recNum) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("降噪确认");
  FONT_CN_16(d);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(18, 42);
  d.printf("降噪 REC_%04d ?", recNum);
  FONT_CN_12(d);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(18, 66);
  d.print("会覆盖原文件");
  drawFooter("回车确认  退格取消");
  waitRelease();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (keyEnter()) { waitRelease(); return true; }
      if (keyDel())   { waitRelease(); return false; }
    }
    delay(8);
  }
}

static bool recVisibleInMode(int recNum, uint8_t mode) {
  if (mode == REC_SHORTCUT) return isShortcutRec(recNum);
  if (mode == REC_IMPORTANT) return isImportantRec(recNum);
  return !isShortcutRec(recNum) && !isImportantRec(recNum);
}

static int firstVisibleIndex(uint8_t mode) {
  for (int i = 0; i < recCount; i++) if (recVisibleInMode(recList[i], mode)) return i;
  return -1;
}

static int countVisible(uint8_t mode) {
  int c = 0;
  for (int i = 0; i < recCount; i++) if (recVisibleInMode(recList[i], mode)) c++;
  return c;
}

static int nearestVisibleIndex(int start, uint8_t mode) {
  if (recCount <= 0) return -1;
  if (start < 0) start = 0;
  if (start >= recCount) start = recCount - 1;
  if (recVisibleInMode(recList[start], mode)) return start;
  for (int d = 1; d < recCount; d++) {
    int a = start - d, b = start + d;
    if (a >= 0 && recVisibleInMode(recList[a], mode)) return a;
    if (b < recCount && recVisibleInMode(recList[b], mode)) return b;
  }
  return -1;
}

static int prevVisibleIndex(int idx, uint8_t mode) {
  for (int i = idx - 1; i >= 0; i--) if (recVisibleInMode(recList[i], mode)) return i;
  return idx;
}

static int nextVisibleIndex(int idx, uint8_t mode) {
  for (int i = idx + 1; i < recCount; i++) if (recVisibleInMode(recList[i], mode)) return i;
  return idx;
}

static void rememberListSelection(uint8_t mode, int sel) {
  if (mode > REC_IMPORTANT || sel < 0 || sel >= recCount) return;
  int recNum = recList[sel];
  if (recVisibleInMode(recNum, mode)) g_listModeSelectedRec[mode] = recNum;
}

static int rememberedListIndex(uint8_t mode) {
  if (mode > REC_IMPORTANT) return -1;
  int recNum = g_listModeSelectedRec[mode];
  int idx = recListIndexOf(recNum);
  if (idx >= 0 && recVisibleInMode(recNum, mode)) return idx;
  g_listModeSelectedRec[mode] = 0;
  return -1;
}

static int selectIndexForMode(uint8_t mode, int fallbackSel) {
  int remembered = rememberedListIndex(mode);
  if (remembered >= 0) return remembered;
  return nearestVisibleIndex(fallbackSel, mode);
}

static void selectRecordedInNormalList(int recNum) {
  g_listMode = REC_NORMAL;
  if (recNum > 0 && recNum <= MAX_REC) g_listModeSelectedRec[REC_NORMAL] = recNum;
}

int listScreen(int selectIdx) {
  auto &d = M5Cardputer.Display;
  if (recCount <= 0 || (selectIdx > 0 && recListIndexOf(selectIdx) < 0)) scanRecordings();

  // 空列表
  if (recCount == 0) {
    d.fillScreen(COL_BG);
    drawBattery();
    FONT_CN_16(d); d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(12, 56); d.print("还没有录音");
    FONT_CN_12(d); d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, 120); d.print("空格录音  Esc息屏");
    waitRelease();
    uint32_t lastInputMs = millis();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastInputMs = millis();
        if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(COL_GREEN); waitRelease(); continue; }
        if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(COL_GREEN); waitRelease(); continue; }
        if (keyBrightUp()) { adjustBrightness(1); drawBrightnessToast(COL_GREEN); waitRelease(); continue; }
        if (keyBrightDn()) { adjustBrightness(-1); drawBrightnessToast(COL_GREEN); waitRelease(); continue; }
        if (keySpace()) return R_RECORD;
        if (keyEsc())   return R_BACK;
        if (keyDel())   return R_BACK;
      }
      systemIdleTick();
      if (autoSleepDue(lastInputMs)) return R_BACK;
      delay(8);
    }
  }

  int sel = recCount - 1;   // 默认选最新录音(列表末尾)
  sel = selectIndexForMode(g_listMode, recCount - 1);
  if (selectIdx > 0) {
    for (int i = 0; i < recCount; i++) {
      if (recList[i] == selectIdx) {
        sel = i;
        g_listMode = recKindOf(selectIdx);
        rememberListSelection(g_listMode, sel);
        break;
      }
    }
  }
  sel = nearestVisibleIndex(sel, g_listMode);
  if (g_carryDeleteRec > 0) {
    int carryIdx = recListIndexOf(g_carryDeleteRec);
    if (carryIdx >= 0) {
      sel = carryIdx;
      g_listMode = recKindOf(g_carryDeleteRec);
      rememberListSelection(g_listMode, sel);
    } else {
      g_carryDeleteRec = 0;
      g_carryDeleteStart = 0;
    }
  }
  bool redraw = true;
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;

      const int rowH = 21;
      const int top = 26;
      const int tagX = 76;
      const int uploadX = 118;
      const int durX = CONTENT_W - 72;
  int visRows = (d.height() - top - 18) / rowH;   // 底部留 18px 给提示
  if (visRows < 1) visRows = 1;

  if (!(g_carryDeleteRec > 0 && keyDel())) waitRelease();
  uint32_t lastInputMs = millis();
  while (true) {
    if (redraw) {
      int visibleTotal = countVisible(g_listMode);
      if (visibleTotal == 0) sel = -1;
      if (visibleTotal > 0 && (sel < 0 || !recVisibleInMode(recList[sel], g_listMode))) sel = nearestVisibleIndex(sel, g_listMode);
      rememberListSelection(g_listMode, sel);
      int selectedOrdinal = 0;
      if (sel >= 0) {
        for (int i = 0; i < sel; i++) if (recVisibleInMode(recList[i], g_listMode)) selectedOrdinal++;
      }
      int firstOrdinal = selectedOrdinal - visRows / 2;
      if (firstOrdinal < 0) firstOrdinal = 0;
      if (firstOrdinal > visibleTotal - visRows) firstOrdinal = visibleTotal - visRows;
      if (firstOrdinal < 0) firstOrdinal = 0;
      redraw = false;
      // 重绘全屏列表内容
      d.fillRect(0, 0, CONTENT_W, 135, COL_BG);

      // 标题行
      drawStatusTabs(d, g_listMode, visibleTotal);

      // 列表行
      if (visibleTotal == 0) {
        d.setTextColor(COL_DIM, COL_BG);
        d.setCursor(28, 58);
        d.print("EMPTY");
      }
      int drawn = 0, ord = 0;
      for (int i = 0; i < recCount && drawn < visRows; i++) {
        if (!recVisibleInMode(recList[i], g_listMode)) continue;
        if (ord++ < firstOrdinal) continue;
        int r = drawn++;
        int y = top + r * rowH;
        bool on = (i == sel);
        uint16_t bg = on ? 0x0180 : COL_BG;
        if (on) d.fillRect(0, y - 2, CONTENT_W, rowH - 1, bg);
        // 选中游标
        if (on) { d.setTextColor(COL_GREEN, bg); d.setCursor(2, y); d.print(">"); }
        d.setTextColor(on ? COL_GREEN : COL_DIM, bg);
        char recName[8];
        snprintf(recName, sizeof(recName), "%04d", recList[i]);
        drawDseg14Text(d, 14, y, recName, on ? COL_GREEN : COL_DIM);
        char hk = hotkeyOf(recList[i]);
        bool imp = isImportantRec(recList[i]);
        if (hk) {
          char keyLabel[8];
          snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
          drawDseg14Text(d, tagX, y, keyLabel, on ? COL_GREEN : COL_DIM);
        } else if (imp) {
          drawDseg14Text(d, tagX, y, "IMP", on ? COL_GREEN : COL_DIM);
        }
        if (uploadDone(recList[i])) {
          drawDseg14Text(d, uploadX, y, "OKK", on ? COL_GREEN : COL_DIM);
        } else if (g_uploadActiveRec == recList[i]) {
          drawDseg14Text(d, uploadX, y, "GO", on ? COL_GREEN : COL_DIM);
        } else if (uploadPending(recList[i]) || uploadModelErr(recList[i]) || uploadJobErr(recList[i])) {
          drawDseg14Text(d, uploadX, y, "OKK", on ? COL_GREEN : COL_DIM);
        } else if (uploadQueued(recList[i])) {
          drawDseg14Text(d, uploadX, y, "WT", on ? COL_GREEN : COL_DIM);
        }
        char dur[8];
        formatDuration(getRecDuration(recList[i]), dur, sizeof(dur));
        d.setTextColor(on ? COL_GREEN : COL_DIM, bg);
        drawDseg14Text(d, durX, y, dur, on ? COL_GREEN : COL_DIM);
        if (frictionPending(recList[i])) d.drawFastHLine(CONTENT_W - 16, y + 15, 6, on ? COL_GREEN : COL_DIM);
      }
      // 更多项箭头
      FONT_CN_12(d); d.setTextColor(COL_DIM, COL_BG);
      if (firstOrdinal > 0)                    { d.setCursor(CONTENT_W - 10, top); d.print("^"); }
      if (firstOrdinal + visRows < visibleTotal) { d.setCursor(CONTENT_W - 10, top + (visRows - 1) * rowH); d.print("v"); }
      // 底部操作提示
      d.drawFastHLine(0, 120, CONTENT_W, COL_DIM);
      d.setCursor(4, 122); d.print(";/.选 回车放 Esc退 长Del删");
    }

    M5Cardputer.update();
    if (g_carryDeleteRec > 0) {
      if (keyDel()) {
        int carryIdx = recListIndexOf(g_carryDeleteRec);
        if (carryIdx >= 0) {
          sel = carryIdx;
          g_listMode = recKindOf(g_carryDeleteRec);
          if (delHoldStart == 0) {
            delHoldStart = g_carryDeleteStart;
            lastDelDraw = 0;
            redraw = true;
          }
        } else {
          g_carryDeleteRec = 0;
          g_carryDeleteStart = 0;
        }
      } else {
        g_carryDeleteRec = 0;
        g_carryDeleteStart = 0;
      }
    }
    if (keyDel() && !keyTab() && sel >= 0) {
      if (delHoldStart == 0) delHoldStart = millis();
      uint32_t held = millis() - delHoldStart;
      if (held >= DELETE_HOLD_MS) {
        int recNum = recList[sel];
        deleteRecording(recNum);
        removeRecListAt(sel);
        g_carryDeleteRec = 0;
        g_carryDeleteStart = 0;
        if (recCount == 0) return R_BACK;
        if (sel >= recCount) sel = recCount - 1;
        sel = nearestVisibleIndex(sel, g_listMode);
        rememberListSelection(g_listMode, sel);
        delHoldStart = 0;
        lastDelDraw = 0;
        redraw = true;
        waitRelease();
      } else if (held >= DELETE_HINT_MS && millis() - lastDelDraw > 60) {
        lastDelDraw = millis();
        drawDeleteProgressOnDisplay(held);
      }
    } else {
      if (delHoldStart != 0) {
        delHoldStart = 0;
        lastDelDraw = 0;
        g_carryDeleteRec = 0;
        g_carryDeleteStart = 0;
        redraw = true;
      }
    }
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      int hk = pressedHotkeyRec();
      if (hk > 0) {                                     // 绑定键=最高优先级, 交给外层播放页
        rememberListSelection(g_listMode, sel);
        g_listMode = REC_SHORTCUT;
        g_listModeSelectedRec[REC_SHORTCUT] = hk;
        g_nextPlay = hk;
        return R_PLAY;
      }
      else if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(COL_GREEN); waitRelease(); }
      else if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(COL_GREEN); waitRelease(); }
      else if (keyBrightUp()) { adjustBrightness(1); drawBrightnessToast(COL_GREEN); waitRelease(); }
      else if (keyBrightDn()) { adjustBrightness(-1); drawBrightnessToast(COL_GREEN); waitRelease(); }
      else if (keyTab() && keyDel()) {
        if (confirmDeleteUnmarked()) {
          int deleted = deleteUnmarkedRecordings();
          sel = recCount - 1;
          g_listMode = REC_NORMAL;
          rememberListSelection(g_listMode, sel);
        }
        redraw = true; waitRelease();
      }
      else if (keyTab() && sel >= 0 && !keyUpload()) {
        if (keyAlt()) {
          int recNum = recList[sel];
          cleanFrictionForRec(recNum, false);
          redraw = true; waitRelease();
          continue;
        }
        if (keyEnter()) {
          bool ok = markRecordingImportant(recList[sel]);
          if (ok) {
            g_listMode = REC_IMPORTANT;
            rememberListSelection(g_listMode, sel);
          }
          redraw = true; waitRelease();
          continue;
        }
        char bk = pressedBindKey();
        if (bk) {
          bool ok = markRecordingShortcut(recList[sel]);
          if (ok) setHotkey(bk, recList[sel]);
          if (ok) {
            g_listMode = REC_SHORTCUT;
            rememberListSelection(g_listMode, sel);
          }
          redraw = true; waitRelease();
        }
      }
      else if (keyTab() && keyUpload()) {
        uint8_t status = UPSTAT_IDLE;
        bool cancelled = false;
        if (sdMount()) {
          int recNum = (sel >= 0) ? recList[sel] : 0;
          cancelled = uploadCancelMounted(recNum);
          SD.end();
        } else {
          status = UPSTAT_NO_SD;
        }
        if (cancelled) status = UPSTAT_ABORTED;
        g_uploadStatus = status;
        drawActionToast(cancelled ? "ABORT" : (status == UPSTAT_NO_SD ? uploadStatusLabel(status) : "NO WT"), cancelled ? COL_GREEN : COL_DIM);
        redraw = true;
        if (M5Cardputer.Keyboard.isPressed()) waitRelease();
      }
      else if (keyUpload() && sel >= 0) {
        if (uploadModeHoldTriggered()) {
          drawActionToast("GO", COL_GREEN);
          waitRelease();
          runUploadBatchMounted(true);
        } else {
          uint8_t status = UPSTAT_NO_SD;
          if (sdMount()) {
            int recNum = recList[sel];
            if (uploadDone(recNum)) {
              status = UPSTAT_DONE;
            } else if (uploadPending(recNum) || uploadModelErr(recNum) || uploadJobErr(recNum)) {
              uploadMarkDone(recNum);
              status = UPSTAT_DONE;
            } else {
              bool queued = enqueueUploadMounted(recNum, recKindOf(recNum));
              status = queued ? validateUploadConfigMounted() : UPSTAT_NO_SD;
              if (status == UPSTAT_IDLE) {
                status = UPSTAT_QUEUED;
                g_uploadStatus = status;
                lastUploadTickMs = millis();
              }
            }
            SD.end();
          }
          g_uploadStatus = status;
          drawActionToast(uploadStatusLabel(status), (status == UPSTAT_QUEUED || status == UPSTAT_DONE) ? COL_GREEN : COL_RED);
        }
        redraw = true;
        if (M5Cardputer.Keyboard.isPressed()) waitRelease();
      }
      else if (keySpace()) { return R_RECORD; }         // 空格=去录音
      else if (keyEsc())   { return R_BACK; }           // Esc=退出列表并息屏
      else if (keyEsc())   { return R_BACK; }
      else if (keyLeft())  { rememberListSelection(g_listMode, sel); g_listMode = (g_listMode + 2) % 3; sel = selectIndexForMode(g_listMode, sel); redraw = true; waitRelease(); }
      else if (keyRight()) { rememberListSelection(g_listMode, sel); g_listMode = (g_listMode + 1) % 3; sel = selectIndexForMode(g_listMode, sel); redraw = true; waitRelease(); }
      else if (keyUp())    { if (sel >= 0) { sel = prevVisibleIndex(sel, g_listMode); rememberListSelection(g_listMode, sel); } redraw = true; }
      else if (keyDown())  { if (sel >= 0) { sel = nextVisibleIndex(sel, g_listMode); rememberListSelection(g_listMode, sel); } redraw = true; }
      else if (keyEnter() && sel >= 0) {
        rememberListSelection(g_listMode, sel);
        g_nextPlay = recList[sel];
        return R_PLAY;
      }
      else if (keyAlt() && sel >= 0) {
        int recNum = recList[sel];
        if (confirmNoiseReduce(recNum)) {
          char p[40]; recordingPathForRec(recNum, p, sizeof(p));
          noiseReduce(p);
        }
        redraw = true; waitRelease();
      }
    }
    if (systemIdleTick()) redraw = true;
    if (autoSleepDue(lastInputMs)) return R_BACK;
    delay(8);
  }
}

// 列表流程: 列表 <-> 录音 循环; Esc退出到息屏
void listFlow(int sel) {
  ensureHotkeysLoaded();
  while (true) {
    int r = listScreen(sel);
    if (r == R_PLAY) {
      int n = g_nextPlay;
      if (n <= 0) continue;
      int pr = playFlow(n);
      if (pr == R_RECORD) {
        int recorded = recordingScreen();
        if (recorded <= 0) {
          if (g_afterRecord == R_LIST) continue;
          return;
        }
        if (g_afterRecord == R_LIST) {
          selectRecordedInNormalList(recorded);
          sel = recorded;
          continue;
        }
        afterRecordingFlow(recorded);
        return;
      }
      if (pr == R_BACK) return;
      if (pr == R_LIST) {
        sel = (g_listReturnRec > 0) ? g_listReturnRec : n;
        g_listReturnRec = 0;
        continue;
      }
      return;
    }
    if (r == R_RECORD) {
      int n = recordingScreen();
      if (n <= 0) {
        if (g_afterRecord == R_LIST) continue;
        return;
      }
      if (g_afterRecord == R_LIST) {
        selectRecordedInNormalList(n);
        sel = n;
        continue;
      }
      afterRecordingFlow(n);
      return;
    }
    return;
  }
}

static void drawWifiField(M5Canvas &cv, int y, const char *label, const char *value, bool selected, bool secret) {
  if (selected) cv.fillRect(0, y - 1, CONTENT_W, 18, COL_DARK_GRAY);
  FONT_CN_16(cv);
  cv.setTextColor(selected ? COL_WHITE : COL_GRAY, selected ? COL_DARK_GRAY : COL_BG);
  cv.setCursor(8, y);
  cv.print(label);
  FONT_ASCII(cv);
  cv.setTextColor(COL_WHITE, selected ? COL_DARK_GRAY : COL_BG);
  cv.setCursor(76, y + 1);
  char shown[23];
  size_t len = strlen(value);
  size_t outLen = min((size_t)22, len);
  for (size_t i = 0; i < outLen; i++) shown[i] = secret ? '*' : value[i];
  shown[outLen] = 0;
  cv.print(shown);
}

static void drawWifiField(m5gfx::M5GFX &g, int y, const char *label, const char *value, bool selected, bool secret) {
  if (selected) g.fillRect(0, y - 1, CONTENT_W, 18, COL_DARK_GRAY);
  FONT_CN_16(g);
  g.setTextColor(selected ? COL_WHITE : COL_GRAY, selected ? COL_DARK_GRAY : COL_BG);
  g.setCursor(8, y);
  g.print(label);
  FONT_ASCII(g);
  g.setTextColor(COL_WHITE, selected ? COL_DARK_GRAY : COL_BG);
  g.setCursor(76, y + 1);
  char shown[23];
  size_t len = strlen(value);
  size_t outLen = min((size_t)22, len);
  for (size_t i = 0; i < outLen; i++) shown[i] = secret ? '*' : value[i];
  shown[outLen] = 0;
  g.print(shown);
}

static char pressedTextChar() {
  for (char c : M5Cardputer.Keyboard.keysState().word) {
    if (c >= 32 && c <= 126) return c;
  }
  return 0;
}

static bool appendTextChar(char *dst, size_t n, char c) {
  if (!dst || n == 0 || !c) return false;
  size_t len = strlen(dst);
  if (len + 1 >= n) return false;
  dst[len] = c;
  dst[len + 1] = 0;
  return true;
}

static bool popTextChar(char *dst) {
  if (!dst) return false;
  size_t len = strlen(dst);
  if (len == 0) return false;
  dst[len - 1] = 0;
  return true;
}

static uint8_t wifiSettingsScreen() {
  if (!sdMount()) {
    showMsg("Wi-Fi", "未检测到 SD 卡", COL_RED);
    return APP_LAUNCHER;
  }
  loadUploadConfig();  // 即使 url/token 不完整，也会尽量读入已有字段。
  SD.end();

  int sel = 0;
  bool redraw = true;
  waitRelease();
  uint32_t lastInputMs = millis();
  M5Canvas cv(&M5Cardputer.Display);
  bool useSprite = cv.createSprite(CONTENT_W, M5Cardputer.Display.height()) != nullptr;
  auto selectedWifiText = [&]() -> char * {
    if (sel == 1) return uploadCfg.ssid;
    if (sel == 2) return uploadCfg.password;
    if (sel == 3) return uploadCfg.ssid2;
    if (sel == 4) return uploadCfg.password2;
    return nullptr;
  };
  auto selectedWifiTextLen = [&]() -> size_t {
    if (sel == 1) return sizeof(uploadCfg.ssid);
    if (sel == 2) return sizeof(uploadCfg.password);
    if (sel == 3) return sizeof(uploadCfg.ssid2);
    if (sel == 4) return sizeof(uploadCfg.password2);
    return 0;
  };
  auto drawWifiScreen = [&]() {
    auto &d = M5Cardputer.Display;
    const char *syncText = uploadCfg.syncEnabled ? "ON" : "OFF";
    if (useSprite) {
      cv.fillScreen(COL_BG);
      cv.fillRect(0, 0, CONTENT_W, 21, COL_BG);
      drawStatusBatteryColor(cv, COL_WHITE);
      drawDseg14Text(cv, 4, 1, "Wi-Fi", COL_WHITE);
      cv.drawFastHLine(0, 20, CONTENT_W, COL_WHITE);
      drawWifiField(cv, 22, "SYNC", syncText, sel == 0, false);
      drawWifiField(cv, 41, "HOME", uploadCfg.ssid, sel == 1, false);
      drawWifiField(cv, 60, "HPW", uploadCfg.password, sel == 2, true);
      drawWifiField(cv, 79, "PHONE", uploadCfg.ssid2, sel == 3, false);
      drawWifiField(cv, 98, "PPW", uploadCfg.password2, sel == 4, true);
      FONT_ASCII(cv);
      cv.setTextColor(COL_GRAY, COL_BG);
      cv.setCursor(6, 122);
      cv.print("Enter save/test  Esc back");
      cv.pushSprite(0, 0);
    } else {
      d.fillScreen(COL_BG);
      d.fillRect(0, 0, CONTENT_W, 21, COL_BG);
      drawStatusBatteryColor(d, COL_WHITE);
      drawDseg14Text(d, 4, 1, "Wi-Fi", COL_WHITE);
      d.drawFastHLine(0, 20, CONTENT_W, COL_WHITE);
      drawWifiField(d, 22, "SYNC", syncText, sel == 0, false);
      drawWifiField(d, 41, "HOME", uploadCfg.ssid, sel == 1, false);
      drawWifiField(d, 60, "HPW", uploadCfg.password, sel == 2, true);
      drawWifiField(d, 79, "PHONE", uploadCfg.ssid2, sel == 3, false);
      drawWifiField(d, 98, "PPW", uploadCfg.password2, sel == 4, true);
      FONT_ASCII(d);
      d.setTextColor(COL_GRAY, COL_BG);
      d.setCursor(6, 122);
      d.print("Enter save/test  Esc back");
    }
  };
  auto drawWifiToast = [&](const char *msg, uint16_t col = COL_GREEN) {
    if (useSprite) {
      drawCanvasToast(cv, msg, col);
    } else {
      drawActionToast(msg, col);
    }
  };
  auto cleanupWifiSprite = [&]() {
    if (useSprite) cv.deleteSprite();
  };

  while (true) {
    M5Cardputer.update();
    if (redraw) {
      drawWifiScreen();
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyEsc()) {
        waitRelease();
        cleanupWifiSprite();
        return APP_LAUNCHER;
      }
      if (keyUp() || keyLeft()) {
        sel = (sel + 4) % 5;
        redraw = true;
        waitRelease();
        continue;
      }
      if (keyDown() || keyRight()) {
        sel = (sel + 1) % 5;
        redraw = true;
        waitRelease();
        continue;
      }
      if (keyDel()) {
        char *field = selectedWifiText();
        if (field) popTextChar(field);
        else uploadCfg.syncEnabled = false;
        redraw = true;
        waitRelease();
        continue;
      }
      if (keyEnter()) {
        if (sel == 0) uploadCfg.syncEnabled = !uploadCfg.syncEnabled;
        bool saved = false;
        if (sdMount()) {
          saved = saveUploadConfigMounted();
          SD.end();
        }
        drawWifiToast(saved ? "SAVED" : "NO SD", saved ? COL_WHITE : COL_RED);
        waitRelease();
#if UPLOAD_WIFI_ENABLED
        if (saved && !uploadCfg.syncEnabled) {
          wifiPowerDown();
          drawWifiToast("SYNC OFF", COL_GRAY);
          delay(700);
        } else if (saved && (uploadCfg.ssid[0] || uploadCfg.ssid2[0])) {
          wifiPowerDown();
          delay(120);
          drawWifiToast("WIFI...", COL_WHITE);
          bool ok = ensureWifiConnected();
          drawWifiToast(ok ? "WIFI OK" : "WIFI ERR", ok ? COL_WHITE : COL_RED);
          delay(900);
        }
#endif
        redraw = true;
        continue;
      }
      char c = pressedTextChar();
      if (c) {
        char *field = selectedWifiText();
        if (field) {
          appendTextChar(field, selectedWifiTextLen(), c);
          redraw = true;
        }
        waitRelease();
      }
    }
    if (autoSleepDue(lastInputMs)) {
      cleanupWifiSprite();
      return APP_SLEEP;
    }
    delay(8);
  }
}

static uint8_t launcherScreen() {
  static const uint16_t LAUNCHER_COL = COL_WHITE;
  static const uint16_t LAUNCHER_DIM = COL_GRAY;
  static const uint16_t LAUNCHER_BG_ON = COL_DARK_GRAY;
  struct LauncherEntry { const char *key; const char *name; uint8_t app; };
  static const LauncherEntry apps[] = {
    {"SPC", "录音", APP_REC_RECORD},
    {"ENT", "列表", APP_REC_LIST},
    {"C",   "CHAT", APP_CHAT},
    {"F",   "番茄钟", APP_POMODORO},
    {"W",   "Wi-Fi", APP_WIFI},
  };
  const int appCount = sizeof(apps) / sizeof(apps[0]);
  int sel = 0;
  bool redraw = true;
  waitRelease();
  uint32_t lastInputMs = millis();

  while (true) {
    M5Cardputer.update();
    if (redraw) {
      auto &d = M5Cardputer.Display;
      d.fillScreen(COL_BG);
      d.fillRect(0, 0, d.width(), 22, COL_BG);
      d.fillRect(2, 5, 3, 13, LAUNCHER_COL);
      FONT_CN_16(d);
      d.setTextColor(LAUNCHER_COL, COL_BG);
      d.setCursor(8, 3);
      d.print("工具箱");
      drawStatusBatteryColor(d, LAUNCHER_COL);
      d.drawFastHLine(0, 21, d.width(), LAUNCHER_COL);
      FONT_ASCII(d);
      for (int i = 0; i < appCount; i++) {
        int y = 27 + i * 20;
        bool on = (i == sel);
        if (on) d.fillRect(0, y - 2, CONTENT_W, 18, LAUNCHER_BG_ON);
        d.setTextColor(on ? LAUNCHER_COL : LAUNCHER_DIM, on ? LAUNCHER_BG_ON : COL_BG);
        d.setCursor(6, y);
        d.print(on ? ">" : " ");
        d.setCursor(24, y);
        d.print(apps[i].key);
        FONT_CN_16(d);
        d.setTextColor(on ? LAUNCHER_COL : LAUNCHER_DIM, on ? LAUNCHER_BG_ON : COL_BG);
        d.setCursor(76, y - 2);
        d.print(apps[i].name);
        FONT_ASCII(d);
      }
      drawFooter(";/.选择  回车进入  Esc息屏", LAUNCHER_DIM);
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyEsc()) { waitRelease(); return APP_LAUNCHER; }
      if (keySpace()) { waitRelease(); return APP_REC_RECORD; }
      if (keyEnter()) { uint8_t app = apps[sel].app; waitRelease(); return app; }
      if (M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F')) {
        waitRelease();
        return APP_POMODORO;
      }
      if (keyChat()) {
        waitRelease();
        return APP_CHAT;
      }
      if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) {
        waitRelease();
        return APP_WIFI;
      }
      if (keyUp()) {
        sel = (sel + appCount - 1) % appCount;
        redraw = true;
      } else if (keyDown()) {
        sel = (sel + 1) % appCount;
        redraw = true;
      }
    }
    systemIdleTick();
    if (autoSleepDue(lastInputMs)) return APP_SLEEP;
    delay(8);
  }
}

static size_t utf8LineBytes(const char *text, size_t start, size_t maxChars, char *line, size_t lineSize) {
  if (!text || !line || lineSize == 0) return start;
  size_t len = strlen(text);
  size_t pos = start;
  size_t out = 0;
  size_t chars = 0;
  while (pos < len && chars < maxChars && out + 4 < lineSize) {
    uint8_t c = (uint8_t)text[pos];
    size_t bytes = 1;
    if ((c & 0x80) == 0) bytes = 1;
    else if ((c & 0xE0) == 0xC0) bytes = 2;
    else if ((c & 0xF0) == 0xE0) bytes = 3;
    else if ((c & 0xF8) == 0xF0) bytes = 4;
    if (pos + bytes > len || out + bytes >= lineSize) break;
    memcpy(line + out, text + pos, bytes);
    out += bytes;
    pos += bytes;
    chars++;
  }
  line[out] = 0;
  return pos;
}

static void drawChatHeader(const char *title = "CHAT");

static void drawChatHome(const char *status, const char *reply = nullptr) {
  auto &d = M5Cardputer.Display;
  uint16_t accent = chatAccentColor();
  uint16_t dim = chatDimColor();
  d.fillScreen(COL_BG);
  drawChatHeader("CHAT");
  FONT_ASCII(d);
  d.setTextColor(accent, COL_BG);
  d.setCursor(8, 31);
  d.print("VOICE CHAT");
  FONT_CN_12(d);
  d.setTextColor(dim, COL_BG);
  d.setCursor(8, 53);
  d.print(status && status[0] ? status : "回车开始说话");
  if (reply && reply[0]) {
    d.setTextColor(accent, COL_BG);
    char line[64];
    size_t off = 0;
    for (int row = 0; row < 3; row++) {
      if (!reply[off]) break;
      off = utf8LineBytes(reply, off, 14, line, sizeof(line));
      d.setCursor(8, 74 + row * 16);
      d.print(line);
    }
  } else {
    d.setTextColor(dim, COL_BG);
    d.setCursor(8, 74);
    d.print("ASR-only 快速返回");
    d.setCursor(8, 94);
    d.print("不等待 DeepSeek");
  }
  drawFooter("回车说话  Space录音  Esc返回", dim);
}

static bool chatRecordOnceMounted(const char *path, uint32_t maxMs = 30000) {
  auto &d = M5Cardputer.Display;
  if (!SD.exists(CHAT_DIR)) SD.mkdir(CHAT_DIR);
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  writeWavHeader(f, REC_RATE, 0);

  bool mustRearm = forceMicRearm;
  bool micWasReady = micInputReady && !mustRearm;
  if (!prepareMicInput(mustRearm)) {
    f.close();
    SD.remove(path);
    return false;
  }
  if (!micWasReady && !mustRearm) delay(20);
  if (mustRearm) {
    for (int i = 0; i < REC_REARM_SETTLE_BUFFERS; i++) {
      M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
      while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    }
    forceMicRearm = false;
  }

  int b = 0;
  size_t recWriteSamples = 0;
  uint32_t dataBytes = 0;
  uint32_t startMs = millis();
  uint32_t lastDraw = 0;
  uint32_t skipBuffers = micWasReady ? 4 : 8;
  bool stop = false;
  bool cancel = false;
  bool ignoreStartKey = M5Cardputer.Keyboard.isPressed();
  int32_t recDc = 0, recHum = 0, recLpf = 0, recNoiseRms = 90, recSoftGateQ8 = 256;

  auto flushRecWrite = [&]() {
    if (recWriteSamples == 0) return;
    size_t bytes = recWriteSamples * sizeof(int16_t);
    f.write((uint8_t *)recWriteBuf, bytes);
    dataBytes += bytes;
    recWriteSamples = 0;
  };

  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  M5Cardputer.Mic.record(recBuf[1], REC_N, REC_RATE);
  d.fillScreen(COL_BG);
  drawChatHeader("CHAT REC");
  drawFooter("回车发送  Esc取消", chatDimColor());

  while (!stop) {
    while (true) {
      M5Cardputer.update();
      if (ignoreStartKey) {
        if (!M5Cardputer.Keyboard.isPressed()) ignoreStartKey = false;
      } else if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyEnter()) { stop = true; waitRelease(); break; }
        if (keyEsc()) { stop = true; cancel = true; waitRelease(); break; }
      }
      if (millis() - startMs >= maxMs) { stop = true; break; }
      if (M5Cardputer.Mic.isRecording() < 2) break;
      delay(1);
    }
    if (stop) break;
    int16_t *filled = recBuf[b];
    processMicBuffer(filled, REC_N, recDc, recHum, recLpf, recNoiseRms, recSoftGateQ8);
    if (skipBuffers > 0) skipBuffers--;
    else {
      memcpy(recWriteBuf + recWriteSamples, filled, REC_N * sizeof(int16_t));
      recWriteSamples += REC_N;
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
    if (recWriteSamples >= REC_WRITE_BATCH * REC_N) flushRecWrite();
    uint32_t now = millis();
    if (now - lastDraw >= 250) {
      lastDraw = now;
      uint32_t s = (now - startMs) / 1000;
      d.fillRect(8, 48, 180, 48, COL_BG);
      FONT_ASCII(d);
      d.setTextColor(chatAccentColor(), COL_BG);
      d.setCursor(8, 52);
      d.printf("REC %02lu/%02lu sec", (unsigned long)s, (unsigned long)(maxMs / 1000));
      FONT_CN_12(d);
      d.setTextColor(chatDimColor(), COL_BG);
      d.setCursor(8, 76);
      d.print("说完按回车发送");
    }
  }

  flushRecWrite();
  uint32_t finalData = dataBytes;
  bool ok = !cancel && finalData >= REC_RATE / 2;
  if (ok) writeWavHeader(f, REC_RATE, finalData);
  f.close();
  while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  micInputReady = false;
  forceMicRearm = true;
  M5Cardputer.Mic.end();
  const uint8_t ES = 0x18;
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  if (!ok) SD.remove(path);
  return ok;
}

static bool makeChatUploadPath(const char *uploadPath, char *chatPath, size_t n) {
  if (!uploadPath || !chatPath || n == 0) return false;
  const char *suffix = "/upload";
  size_t len = strlen(uploadPath);
  size_t suffixLen = strlen(suffix);
  if (len >= suffixLen && strcmp(uploadPath + len - suffixLen, suffix) == 0) {
    size_t baseLen = len - suffixLen;
    if (baseLen + strlen("/chat/upload") >= n) return false;
    memcpy(chatPath, uploadPath, baseLen);
    chatPath[baseLen] = 0;
    strlcat(chatPath, "/chat/upload", n);
    return true;
  }
  return strlcpy(chatPath, "/chat/upload", n) < n;
}

static bool makeChatStreamPath(const char *uploadPath, char *streamPath, size_t n) {
  if (!uploadPath || !streamPath || n == 0) return false;
  (void)uploadPath;
  return strlcpy(streamPath, "/chat/stream", n) < n;
}

static void chatPcmPumpPlayback();

static bool readWsFrame(WiFiClient &client, uint8_t *payload, size_t payloadSize, size_t &payloadLen, uint8_t &opcode, uint32_t timeoutMs) {
  payloadLen = 0;
  opcode = 0;
  uint32_t start = millis();
  while (client.connected() && client.available() < 2 && millis() - start < timeoutMs) {
    M5Cardputer.update();
    chatPcmPumpPlayback();
    delay(1);
  }
  if (client.available() < 2) return false;

  uint8_t b0 = client.read();
  uint8_t b1 = client.read();
  opcode = b0 & 0x0f;
  bool masked = (b1 & 0x80) != 0;
  uint32_t len = b1 & 0x7f;
  if (len == 126) {
    while (client.available() < 2 && millis() - start < timeoutMs) {
      M5Cardputer.update();
      chatPcmPumpPlayback();
      delay(1);
    }
    if (client.available() < 2) return false;
    len = ((uint32_t)client.read() << 8) | (uint32_t)client.read();
  } else if (len == 127) {
    return false;
  }

  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    while (client.available() < 4 && millis() - start < timeoutMs) {
      M5Cardputer.update();
      chatPcmPumpPlayback();
      delay(1);
    }
    if (client.available() < 4) return false;
    client.read(mask, 4);
  }

  payloadLen = len < payloadSize ? len : payloadSize;
  if (!masked) {
    uint32_t kept = 0;
    uint8_t discard[64];
    while (kept < len) {
      uint8_t *dst = kept < payloadSize ? payload + kept : discard;
      uint32_t remaining = len - kept;
      size_t cap = kept < payloadSize ? payloadSize - kept : sizeof(discard);
      size_t want = remaining < cap ? (size_t)remaining : cap;
      while (client.available() <= 0 && client.connected() && millis() - start < timeoutMs) {
        M5Cardputer.update();
        chatPcmPumpPlayback();
        delay(1);
      }
      if (!client.available()) return false;
      size_t avail = (size_t)client.available();
      int got = client.read(dst, want < avail ? want : avail);
      if (got <= 0) return false;
      kept += (uint32_t)got;
      chatPcmPumpPlayback();
    }
    return true;
  }
  for (uint32_t i = 0; i < len; i++) {
    while (!client.available() && client.connected() && millis() - start < timeoutMs) {
      M5Cardputer.update();
      chatPcmPumpPlayback();
      delay(1);
    }
    if (!client.available()) return false;
    uint8_t c = client.read();
    if (masked) c ^= mask[i & 3];
    if (i < payloadSize) payload[i] = c;
    if ((i & 0x7f) == 0) chatPcmPumpPlayback();
  }
  return true;
}

static bool readWsTextFrame(WiFiClient &client, char *out, size_t n, uint32_t timeoutMs) {
  if (!out || n == 0) return false;
  out[0] = 0;
  uint8_t frame[256];
  size_t len = 0;
  uint8_t opcode = 0;
  if (!readWsFrame(client, frame, sizeof(frame) - 1, len, opcode, timeoutMs)) return false;
  if (opcode != 0x01 || len == 0) return false;
  frame[len] = 0;
  strlcpy(out, (const char *)frame, n);
  return true;
}

static bool writeWsFrame(WiFiClient &client, uint8_t opcode, const uint8_t *payload, size_t len) {
  if (!client.connected()) return false;
  static uint8_t txFrame[1400];
  if (len + 8 > sizeof(txFrame)) return false;
  size_t h = 0;
  txFrame[h++] = 0x80 | (opcode & 0x0f);
  if (len < 126) {
    txFrame[h++] = 0x80 | (uint8_t)len;
  } else if (len <= 0xffff) {
    txFrame[h++] = 0x80 | 126;
    txFrame[h++] = (uint8_t)(len >> 8);
    txFrame[h++] = (uint8_t)len;
  } else return false;
  uint32_t seed = millis() ^ ((uint32_t)len << 11) ^ ((uint32_t)opcode << 24);
  uint8_t *mask = txFrame + h;
  mask[0] = (uint8_t)(seed >> 24);
  mask[1] = (uint8_t)(seed >> 16);
  mask[2] = (uint8_t)(seed >> 8);
  mask[3] = (uint8_t)seed;
  h += 4;
  for (size_t i = 0; i < len; i++) {
    txFrame[h + i] = payload ? (payload[i] ^ mask[i & 3]) : mask[i & 3];
  }
  size_t total = h + len;
  return client.write(txFrame, total) == total;
}

static bool writeWsText(WiFiClient &client, const char *text) {
  return writeWsFrame(client, 0x01, (const uint8_t *)text, text ? strlen(text) : 0);
}

static bool writeWsBinary(WiFiClient &client, const uint8_t *payload, size_t len) {
  return writeWsFrame(client, 0x02, payload, len);
}

#if UPLOAD_WIFI_ENABLED
static bool chatStreamConnectMounted(WiFiClient &client, char *detail, size_t detailLen) {
  if (detail && detailLen) detail[0] = 0;
  auto setDetail = [&](const char *s) {
    if (detail && detailLen) strlcpy(detail, s ? s : "", detailLen);
  };

  if (!loadUploadConfig()) { setDetail("NO CFG"); return false; }
  if (!uploadCfg.syncEnabled) { wifiPowerDown(); setDetail("SYNC OFF"); return false; }
  if (!ensureWifiConnected()) { setDetail("WIFI ERR"); return false; }

  char host[65], uploadPath[96], streamPath[96], hostHeader[72];
  uint16_t port = 80;
  if (!parseHttpUrl(uploadCfg.url, host, sizeof(host), port, uploadPath, sizeof(uploadPath)) ||
      !makeChatStreamPath(uploadPath, streamPath, sizeof(streamPath))) {
    wifiPowerDown();
    setDetail("BAD URL");
    return false;
  }
  if (port == 80) snprintf(hostHeader, sizeof(hostHeader), "%s", host);
  else snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);

  client.setTimeout(12000);
  if (!client.connect(host, port, 5000)) {
    wifiPowerDown();
    setDetail("CONNECT ERR");
    return false;
  }
  client.setNoDelay(true);
  client.printf("GET %s HTTP/1.1\r\n", streamPath);
  client.printf("Host: %s\r\n", hostHeader);
  client.print("Upgrade: websocket\r\n");
  client.print("Connection: Upgrade\r\n");
  client.print("Sec-WebSocket-Version: 13\r\n");
  client.print("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
  client.printf("X-Upload-Token: %s\r\n", uploadCfg.token);
  client.printf("X-Device-Id: %s\r\n", uploadCfg.device);
  client.printf("X-Wifi-Rssi: %d\r\n", WiFi.RSSI());
  client.print("\r\n");

  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 12000) {
    M5Cardputer.update();
    delay(20);
  }
  if (!client.available()) {
    client.stop();
    wifiPowerDown();
    setDetail("NO RESPONSE");
    return false;
  }

  String status = client.readStringUntil('\n');
  int sp = status.indexOf(' ');
  int code = sp >= 0 ? status.substring(sp + 1).toInt() : 0;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }
  if (code != 101) {
    client.stop();
    wifiPowerDown();
    if (detail && detailLen) snprintf(detail, detailLen, "HTTP %d", code);
    return false;
  }

  char hello[192];
  if (!readWsTextFrame(client, hello, sizeof(hello), 8000)) {
    client.stop();
    wifiPowerDown();
    setDetail("NO HELLO");
    return false;
  }
  if (strstr(hello, "connected") == nullptr) {
    client.stop();
    wifiPowerDown();
    setDetail("BAD HELLO");
    return false;
  }
  setDetail("CONNECTED");
  return true;
}
#else
static bool chatStreamConnectMounted(WiFiClient &client, char *detail, size_t detailLen) {
  (void)client;
  if (detail && detailLen) strlcpy(detail, "NO WIFI", detailLen);
  return false;
}
#endif

static bool extractJsonStringValue(const String &json, const char *key, char *out, size_t n) {
  if (!out || n == 0) return false;
  out[0] = 0;
  String needle = String("\"") + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return false;
  p = json.indexOf(':', p);
  if (p < 0) return false;
  p = json.indexOf('"', p);
  if (p < 0) return false;
  int e = p + 1;
  size_t w = 0;
  bool esc = false;
  while (e < json.length() && w + 1 < n) {
    char c = json[e++];
    if (esc) {
      if (c == 'n') out[w++] = '\n';
      else if (c == 'r') out[w++] = '\n';
      else if (c == 't') out[w++] = ' ';
      else out[w++] = c;
      esc = false;
      continue;
    }
    if (c == '\\') { esc = true; continue; }
    if (c == '"') break;
    out[w++] = c;
  }
  out[w] = 0;
  return w > 0;
}

static bool downloadHttpToFileMounted(const char *url, const char *path) {
  if (!url || !url[0] || !path || !path[0]) return false;
  if (!loadUploadConfig()) return false;
  if (!ensureWifiConnected()) return false;

  char host[65], reqPath[256], hostHeader[72];
  uint16_t port = 80;
  if (!parseHttpUrl(url, host, sizeof(host), port, reqPath, sizeof(reqPath))) {
    wifiPowerDown();
    return false;
  }
  if (port == 80) snprintf(hostHeader, sizeof(hostHeader), "%s", host);
  else snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);

  File out = SD.open(path, FILE_WRITE);
  if (!out) {
    wifiPowerDown();
    return false;
  }
  WiFiClient client;
  client.setTimeout(15000);
  if (!client.connect(host, port, 4000)) {
    out.close();
    SD.remove(path);
    wifiPowerDown();
    return false;
  }
  client.setNoDelay(true);
  client.printf("GET %s HTTP/1.1\r\n", reqPath);
  client.printf("Host: %s\r\n", hostHeader);
  client.print("Connection: close\r\n\r\n");

  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 20000) {
    M5Cardputer.update();
    if (keyEsc()) {
      client.stop();
      out.close();
      SD.remove(path);
      wifiPowerDown();
      return false;
    }
    delay(20);
  }

  int code = 0;
  int contentLength = -1;
  if (client.available()) {
    String status = client.readStringUntil('\n');
    int sp = status.indexOf(' ');
    if (sp >= 0) code = status.substring(sp + 1).toInt();
    while (client.connected() || client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("content-length:")) {
        contentLength = lower.substring(15).toInt();
      }
    }
  }
  if (code < 200 || code >= 300) {
    client.stop();
    out.close();
    SD.remove(path);
    wifiPowerDown();
    return false;
  }

  static uint8_t buf[UPLOAD_CHUNK_BYTES];
  int received = 0;
  while (client.connected() || client.available()) {
    int avail = client.available();
    if (avail > 0) {
      int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
      int got = client.read(buf, want);
      if (got > 0) {
        out.write(buf, got);
        received += got;
      }
    } else {
      delay(5);
    }
    M5Cardputer.update();
    if (keyEsc()) {
      client.stop();
      out.close();
      SD.remove(path);
      wifiPowerDown();
      return false;
    }
  }
  client.stop();
  out.flush();
  out.close();
  wifiPowerDown();
  if (received <= 44 || (contentLength > 0 && received < contentLength)) {
    SD.remove(path);
    return false;
  }
  return true;
}

static bool playWavQuickMounted(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t dataBytes = playableWavDataBytes(f);
  if (!dataBytes || !f.seek(44)) {
    f.close();
    return false;
  }
  uint32_t rate = wavSampleRate(f);
  f.seek(44);
  speakerOn();
  static int16_t pcm[PB_N];
  uint32_t remaining = dataBytes;
  while (remaining > 0) {
    size_t want = remaining > sizeof(pcm) ? sizeof(pcm) : remaining;
    want &= ~1U;
    if (!want) break;
    size_t got = f.read((uint8_t *)pcm, want);
    if (got == 0) break;
    size_t samples = got / 2;
    while (M5Cardputer.Speaker.isPlaying(0) >= 2) {
      M5Cardputer.update();
      if (keyEsc()) {
        M5Cardputer.Speaker.stop();
        f.close();
        return false;
      }
      delay(2);
    }
    M5Cardputer.Speaker.playRaw(pcm, samples, rate, false, 1, 0, false);
    remaining -= got;
  }
  while (M5Cardputer.Speaker.isPlaying(0)) {
    M5Cardputer.update();
    if (keyEsc()) break;
    delay(5);
  }
  M5Cardputer.Speaker.stop();
  f.close();
  return true;
}

#if UPLOAD_WIFI_ENABLED
static uint8_t uploadChatWavMounted(const char *path, char *reply, size_t replyLen, char *audioUrl, size_t audioUrlLen) {
  if (reply && replyLen) reply[0] = 0;
  if (audioUrl && audioUrlLen) audioUrl[0] = 0;
  if (!loadUploadConfig()) return UPSTAT_NO_CFG;
  if (!uploadCfg.syncEnabled) { wifiPowerDown(); return UPSTAT_SYNC_OFF; }
  if (!ensureWifiConnected()) return UPSTAT_WIFI_ERR;

  File f = SD.open(path, FILE_READ);
  if (!f || f.size() <= 44) {
    if (f) f.close();
    wifiPowerDown();
    return UPSTAT_NO_FILE;
  }
  uint32_t bodyBytes = f.size();
  char host[65], uploadPath[96], chatPath[96], hostHeader[72];
  uint16_t port = 80;
  if (!parseHttpUrl(uploadCfg.url, host, sizeof(host), port, uploadPath, sizeof(uploadPath)) ||
      !makeChatUploadPath(uploadPath, chatPath, sizeof(chatPath))) {
    f.close();
    wifiPowerDown();
    return UPSTAT_BAD_URL;
  }
  if (port == 80) snprintf(hostHeader, sizeof(hostHeader), "%s", host);
  else snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);

  char recordedAt[24] = {0};
  boxFormatNow(recordedAt, sizeof(recordedAt));
  WiFiClient client;
  client.setTimeout(15000);
  if (!client.connect(host, port, 4000)) {
    f.close();
    wifiPowerDown();
    return UPSTAT_HTTP_ERR;
  }
  client.setNoDelay(true);
  client.printf("POST %s HTTP/1.1\r\n", chatPath);
  client.printf("Host: %s\r\n", hostHeader);
  client.print("Connection: close\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)bodyBytes);
  client.printf("X-Upload-Token: %s\r\n", uploadCfg.token);
  client.printf("X-Device-Id: %s\r\n", uploadCfg.device);
  client.printf("X-Wifi-Rssi: %d\r\n", WiFi.RSSI());
  IPAddress localIp = WiFi.localIP();
  client.printf("X-Wifi-IP: %u.%u.%u.%u\r\n", localIp[0], localIp[1], localIp[2], localIp[3]);
  if (recordedAt[0]) client.printf("X-Recorded-At: %s\r\n", recordedAt);
  client.print("X-Recording-Name: CHAT_LAST.wav\r\n\r\n");

  static uint8_t buf[UPLOAD_CHUNK_BYTES];
  while (f.available()) {
    size_t got = f.read(buf, sizeof(buf));
    if (got == 0) break;
    if (client.write(buf, got) != got) {
      client.stop();
      f.close();
      wifiPowerDown();
      return UPSTAT_HTTP_ERR;
    }
    M5Cardputer.update();
    if (keyEsc()) {
      client.stop();
      f.close();
      wifiPowerDown();
      return UPSTAT_ABORTED;
    }
    delay(0);
  }

  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 90000) {
    M5Cardputer.update();
    if (keyEsc()) {
      client.stop();
      f.close();
      wifiPowerDown();
      return UPSTAT_ABORTED;
    }
    delay(30);
  }
  int code = 0;
  String body;
  if (client.available()) {
    String status = client.readStringUntil('\n');
    int sp = status.indexOf(' ');
    if (sp >= 0) code = status.substring(sp + 1).toInt();
    while (client.connected() || client.available()) {
      String line = client.readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    }
    body = client.readString();
  }
  client.stop();
  f.close();
  wifiPowerDown();
  if (code >= 200 && code < 300) {
    if (reply && replyLen) {
      if (!extractJsonStringValue(body, "replyText", reply, replyLen)) {
        extractJsonStringValue(body, "userText", reply, replyLen);
      }
      if (audioUrl && audioUrlLen) {
        extractJsonStringValue(body, "audioUrl", audioUrl, audioUrlLen);
      }
    }
    return UPSTAT_DONE;
  }
  return UPSTAT_HTTP_ERR;
}
#else
static uint8_t uploadChatWavMounted(const char *path, char *reply, size_t replyLen, char *audioUrl, size_t audioUrlLen) {
  (void)path;
  if (reply && replyLen) reply[0] = 0;
  if (audioUrl && audioUrlLen) audioUrl[0] = 0;
  return UPSTAT_NO_CFG;
}
#endif

static const size_t CHAT_PCM_RING_SAMPLES = 24576;
static const size_t CHAT_PCM_START_SAMPLES = 8192;
static const size_t CHAT_PCM_START_MIN_SAMPLES = 6144;
static const size_t CHAT_PCM_START_MAX_SAMPLES = 16384;
static const size_t CHAT_PLAY_SAMPLES = 512;
static int16_t *chatPcmRing = nullptr;
static size_t chatPcmHead = 0;
static size_t chatPcmTail = 0;
static size_t chatPcmCount = 0;
static portMUX_TYPE chatPcmMux = portMUX_INITIALIZER_UNLOCKED;
static int16_t chatPlayBuf[6][CHAT_PLAY_SAMPLES];
static uint8_t chatPlayBufIndex = 0;
static bool chatPcmStarted = false;
static bool chatAudioStreaming = false;
static bool chatDeferPlayback = false;
static bool chatTtsPlaying = false;
static bool chatReadyAfterPlayback = false;
static volatile bool chatPlaybackTaskRun = false;
static TaskHandle_t chatPlaybackTaskHandle = nullptr;
static uint32_t chatBinaryFrames = 0;
static uint32_t chatBinaryBytes = 0;
static uint32_t chatDropSamples = 0;
static uint32_t chatUnderruns = 0;
static size_t chatPcmStartTarget = CHAT_PCM_START_SAMPLES;
static bool chatTtsHadUnderrun = false;
static bool chatShowDebugStats = false;
static bool chatTextPageVisible = false;
static bool chatUplinkActive = false;
static bool chatUplinkQueued = false;
static uint32_t chatUplinkChunks = 0;
static uint32_t chatUplinkStartedAt = 0;
static uint16_t chatUplinkLevel = 0;
static int32_t chatUplinkDc = 0;
static int32_t chatUplinkHum = 0;
static int32_t chatUplinkLpf = 0;
static int32_t chatUplinkNoiseRms = 90;
static int32_t chatUplinkSoftGateQ8 = 256;
static const uint32_t CHAT_TAP_CANCEL_MS = 320;
static const uint32_t CHAT_MAX_UPLINK_MS = 20000;
static const uint16_t CHAT_COL = 0x04FF;
static const uint16_t CHAT_DIM = 0x0252;
static const uint16_t CHAT_ACCENT_COLORS[] = {CHAT_COL, 0x04BF, 0x039F, 0x681F, 0x07FF};
static uint8_t chatAccentIndex = 0;

static uint16_t chatAccentColor() {
  return CHAT_ACCENT_COLORS[chatAccentIndex % (sizeof(CHAT_ACCENT_COLORS) / sizeof(CHAT_ACCENT_COLORS[0]))];
}

static uint16_t chatDimColor() {
  uint16_t col = chatAccentColor();
  uint16_t r = (col >> 11) & 0x1f;
  uint16_t g = (col >> 5) & 0x3f;
  uint16_t b = col & 0x1f;
  r = max<uint16_t>(1, (r * 2) / 5);
  g = max<uint16_t>(2, (g * 2) / 5);
  b = max<uint16_t>(1, (b * 2) / 5);
  return (r << 11) | (g << 5) | b;
}

static void drawChatBattery() {
  auto &d = M5Cardputer.Display;
  int bat = filteredBatteryLevel();
  uint16_t col = (bat <= 20) ? COL_RED : chatAccentColor();
  d.fillRect(CONTENT_W - 62, 0, 62, 18, COL_BG);
  char text[8];
  snprintf(text, sizeof(text), "%d%%", bat);
  drawDseg14Text(d, CONTENT_W - dseg14TextWidth(text) - 4, 1, text, col);
}

static void drawChatHeader(const char *title) {
  auto &d = M5Cardputer.Display;
  uint16_t col = chatAccentColor();
  d.fillRect(0, 0, d.width(), 22, COL_BG);
  d.fillRect(2, 5, 3, 13, col);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(8, 4);
  d.print(title && title[0] ? title : "CHAT");
  drawChatBattery();
  d.drawFastHLine(0, 21, d.width(), col);
}

static bool chatPcmAlloc() {
  if (chatPcmRing) return true;
  chatPcmRing = (int16_t *)malloc(CHAT_PCM_RING_SAMPLES * sizeof(int16_t));
  return chatPcmRing != nullptr;
}

static void chatPcmFree() {
  free(chatPcmRing);
  chatPcmRing = nullptr;
}

static void chatPcmReset(bool keepTextPage = false) {
  portENTER_CRITICAL(&chatPcmMux);
  chatPcmHead = 0;
  chatPcmTail = 0;
  chatPcmCount = 0;
  portEXIT_CRITICAL(&chatPcmMux);
  chatPlayBufIndex = 0;
  chatPcmStarted = false;
  chatAudioStreaming = false;
  chatDeferPlayback = false;
  chatTtsPlaying = false;
  chatReadyAfterPlayback = false;
  chatBinaryFrames = 0;
  chatBinaryBytes = 0;
  chatDropSamples = 0;
  chatUnderruns = 0;
  chatTtsHadUnderrun = false;
  if (!keepTextPage) chatTextPageVisible = false;
  chatUplinkActive = false;
  chatUplinkQueued = false;
  chatUplinkChunks = 0;
  chatUplinkStartedAt = 0;
  chatUplinkLevel = 0;
  chatUplinkDc = 0;
  chatUplinkHum = 0;
  chatUplinkLpf = 0;
  chatUplinkNoiseRms = 90;
  chatUplinkSoftGateQ8 = 256;
}

static bool chatPcmPushSample(int16_t sample) {
  if (!chatPcmRing) return false;
  bool ok = false;
  portENTER_CRITICAL(&chatPcmMux);
  if (chatPcmCount >= CHAT_PCM_RING_SAMPLES) {
    portEXIT_CRITICAL(&chatPcmMux);
    chatDropSamples++;
    return false;
  }
  chatPcmRing[chatPcmHead] = sample;
  chatPcmHead = (chatPcmHead + 1) % CHAT_PCM_RING_SAMPLES;
  chatPcmCount++;
  ok = true;
  portEXIT_CRITICAL(&chatPcmMux);
  return ok;
}

static void chatPcmPushBytes(const uint8_t *data, size_t len) {
  if (!data || !chatPcmRing) return;
  len &= ~1U;
  size_t samples = len / 2;
  size_t pushed = 0;
  portENTER_CRITICAL(&chatPcmMux);
  while (pushed < samples && chatPcmCount < CHAT_PCM_RING_SAMPLES) {
    size_t i = pushed * 2;
    chatPcmRing[chatPcmHead] = (int16_t)((uint16_t)data[i] | ((uint16_t)data[i + 1] << 8));
    chatPcmHead = (chatPcmHead + 1) % CHAT_PCM_RING_SAMPLES;
    chatPcmCount++;
    pushed++;
  }
  portEXIT_CRITICAL(&chatPcmMux);
  if (pushed < samples) chatDropSamples += samples - pushed;
}

static bool chatPcmPopBlock(int16_t *out, size_t samples) {
  if (!chatPcmRing || !out || chatPcmCount < samples) return false;
  portENTER_CRITICAL(&chatPcmMux);
  if (chatPcmCount < samples) {
    portEXIT_CRITICAL(&chatPcmMux);
    return false;
  }
  for (size_t i = 0; i < samples; i++) {
    out[i] = chatPcmRing[chatPcmTail];
    chatPcmTail = (chatPcmTail + 1) % CHAT_PCM_RING_SAMPLES;
    chatPcmCount--;
  }
  portEXIT_CRITICAL(&chatPcmMux);
  return true;
}

static size_t chatPcmAvailable() {
  portENTER_CRITICAL(&chatPcmMux);
  size_t n = chatPcmCount;
  portEXIT_CRITICAL(&chatPcmMux);
  return n;
}

static void chatPlayReferenceTone() {
  static int16_t refBuf[3][CHAT_PLAY_SAMPLES];
  const uint32_t sampleRate = REC_RATE;
  const uint32_t totalSamples = sampleRate * 1200 / 1000;
  uint8_t bi = 0;
  speakerOn();
  for (uint32_t offset = 0; offset < totalSamples; offset += CHAT_PLAY_SAMPLES) {
    size_t n = totalSamples - offset;
    if (n > CHAT_PLAY_SAMPLES) n = CHAT_PLAY_SAMPLES;
    while (M5Cardputer.Speaker.isPlaying(0) >= 2) {
      M5Cardputer.update();
      delay(2);
    }
    int16_t *out = refBuf[bi];
    for (size_t i = 0; i < n; i++) {
      uint32_t pos = offset + i;
      float fadeIn = pos < 800 ? (float)pos / 800.0f : 1.0f;
      float fadeOut = (totalSamples - pos) < 800 ? (float)(totalSamples - pos) / 800.0f : 1.0f;
      float env = fadeIn < fadeOut ? fadeIn : fadeOut;
      float t = (float)pos / (float)sampleRate;
      out[i] = (int16_t)roundf(sinf(2.0f * PI * 660.0f * t) * 5200.0f * env);
    }
    M5Cardputer.Speaker.playRaw(out, n, sampleRate, false, 1, 0, false);
    bi = (bi + 1) % 3;
  }
  while (M5Cardputer.Speaker.isPlaying(0)) {
    M5Cardputer.update();
    delay(5);
  }
  M5Cardputer.Speaker.stop();
}

static void chatPcmPumpPlayback() {
  if (chatDeferPlayback && chatAudioStreaming) return;
  if (!chatPcmStarted) {
    if (chatPcmAvailable() < chatPcmStartTarget) return;
    chatPcmStarted = true;
  }
  size_t available = chatPcmAvailable();
  if (chatAudioStreaming && available < CHAT_PLAY_SAMPLES && M5Cardputer.Speaker.isPlaying(0) == 0) {
    chatUnderruns++;
    chatTtsHadUnderrun = true;
  }
  while (available >= CHAT_PLAY_SAMPLES && M5Cardputer.Speaker.isPlaying(0) < 5) {
    int16_t *out = chatPlayBuf[chatPlayBufIndex];
    if (!chatPcmPopBlock(out, CHAT_PLAY_SAMPLES)) return;
    M5Cardputer.Speaker.playRaw(out, CHAT_PLAY_SAMPLES, REC_RATE, false, 1, 0, false);
    chatPlayBufIndex = (chatPlayBufIndex + 1) % 6;
    available = chatPcmAvailable();
  }
}

static void chatTunePlaybackStartBuffer() {
  if (chatTtsHadUnderrun || chatDropSamples > 0) {
    if (chatPcmStartTarget < CHAT_PCM_START_MAX_SAMPLES) {
      chatPcmStartTarget += 2048;
      if (chatPcmStartTarget > CHAT_PCM_START_MAX_SAMPLES) chatPcmStartTarget = CHAT_PCM_START_MAX_SAMPLES;
    }
  } else if (chatPcmStartTarget > CHAT_PCM_START_MIN_SAMPLES) {
    chatPcmStartTarget -= 1024;
    if (chatPcmStartTarget < CHAT_PCM_START_MIN_SAMPLES) chatPcmStartTarget = CHAT_PCM_START_MIN_SAMPLES;
  }
  chatTtsHadUnderrun = false;
}

static void chatPlaybackTask(void *arg) {
  (void)arg;
  while (chatPlaybackTaskRun) {
    chatPcmPumpPlayback();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  chatPlaybackTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void chatStartPlaybackTask() {
  if (chatPlaybackTaskHandle) return;
  chatPlaybackTaskRun = true;
  xTaskCreatePinnedToCore(chatPlaybackTask, "chat_play", 4096, nullptr, 2, &chatPlaybackTaskHandle, 1);
}

static void chatStopPlaybackTask() {
  if (!chatPlaybackTaskHandle) return;
  chatPlaybackTaskRun = false;
  for (int i = 0; i < 50 && chatPlaybackTaskHandle; i++) delay(1);
  chatPlaybackTaskHandle = nullptr;
}

static void drawChatStreamStats() {
  if (!chatShowDebugStats) return;
  auto &d = M5Cardputer.Display;
  FONT_ASCII(d);
  d.fillRect(0, 84, CONTENT_W, 44, COL_BG);
  d.setTextColor(chatDimColor(), COL_BG);
  d.setCursor(8, 88);
  d.printf("BIN %lu  BUF %u", (unsigned long)chatBinaryFrames, (unsigned)chatPcmAvailable());
  d.setCursor(8, 106);
  d.printf("UND %lu  DROP %lu", (unsigned long)chatUnderruns, (unsigned long)chatDropSamples);
}

static void drawChatStreamHome(const char *status, uint16_t col);

static void drawChatHintLine(const char *text, int y, uint16_t col) {
  if (!text || !text[0]) return;
  auto &d = M5Cardputer.Display;
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  int w = d.textWidth(text);
  int x = (CONTENT_W - w) / 2;
  if (x < 0) x = 0;
  d.setCursor(x, y);
  d.print(text);
}

static void drawChatListenMeter(uint32_t elapsedMs, uint16_t level) {
  auto &d = M5Cardputer.Display;
  d.fillRect(0, 78, 172, 34, COL_BG);
  FONT_ASCII(d);
  d.setTextColor(chatAccentColor(), COL_BG);
  char secText[16];
  snprintf(secText, sizeof(secText), "REC %lus", (unsigned long)(elapsedMs / 1000));
  d.setCursor(8, 84);
  d.print(secText);
  const int barX = 8;
  const int barY = 104;
  const int barW = 150;
  const int barH = 6;
  int fillW = map((int)min<uint16_t>(level, 9000), 0, 9000, 2, barW - 2);
  d.drawRect(barX, barY, barW, barH, chatDimColor());
  d.fillRect(barX + 1, barY + 1, fillW, barH - 2, chatAccentColor());
}

static size_t chatUtf8CharLen(unsigned char c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

static void drawChatStreamWrappedText(const char *text) {
  auto &d = M5Cardputer.Display;
  FONT_CN_12(d);
  d.setTextColor(chatAccentColor(), COL_BG);
  const int x = 8;
  const int maxW = CONTENT_W - 16;
  const int lineH = 16;
  int y = 42;
  char line[96];
  size_t lineLen = 0;
  line[0] = 0;

  const char *p = text && text[0] ? text : "(empty)";
  while (*p && y <= 122) {
    size_t n = chatUtf8CharLen((unsigned char)*p);
    if (n > 4) n = 1;
    char ch[5] = {0};
    for (size_t i = 0; i < n && p[i]; i++) ch[i] = p[i];
    if (ch[0] == '\r' || ch[0] == '\n') ch[0] = ' ', ch[1] = 0, n = 1;

    char candidate[sizeof(line)];
    strlcpy(candidate, line, sizeof(candidate));
    if (lineLen + n + 1 < sizeof(candidate)) strlcat(candidate, ch, sizeof(candidate));

    if (lineLen > 0 && d.textWidth(candidate) > maxW) {
      d.setCursor(x, y);
      d.print(line);
      y += lineH;
      line[0] = 0;
      lineLen = 0;
      continue;
    }
    if (lineLen + n + 1 < sizeof(line)) {
      strlcat(line, ch, sizeof(line));
      lineLen += n;
    }
    p += n;
  }
  if (lineLen > 0 && y <= 122) {
    d.setCursor(x, y);
    d.print(line);
  }
}

static bool chatLooksAsciiArt(const char *text) {
  if (!text) return false;
  size_t ascii = 0, total = 0, lines = 0;
  for (const char *p = text; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') lines++;
    if (c >= 32 && c <= 126) ascii++;
    if (c != '\r' && c != '\n') total++;
  }
  return lines >= 2 && total > 12 && ascii * 100 / total >= 90;
}

static void drawChatStreamAsciiText(const char *text) {
  auto &d = M5Cardputer.Display;
  FONT_ASCII(d);
  d.setTextColor(chatAccentColor(), COL_BG);
  int y = 40;
  char line[40];
  size_t n = 0;
  line[0] = 0;
  const char *p = text && text[0] ? text : "(empty)";
  while (*p && y <= 122) {
    char c = *p++;
    if (c == '\r') continue;
    if (c == '\n' || n >= sizeof(line) - 1) {
      line[n] = 0;
      d.setCursor(8, y);
      d.print(line);
      y += 12;
      n = 0;
      line[0] = 0;
      if (c == '\n') continue;
    }
    if (c >= 32 && c <= 126 && n < sizeof(line) - 1) line[n++] = c;
  }
  if (n > 0 && y <= 122) {
    line[n] = 0;
    d.setCursor(8, y);
    d.print(line);
  }
}

static void drawChatStreamTextPage(const char *title, const char *text) {
  auto &d = M5Cardputer.Display;
  chatTextPageVisible = true;
  d.fillScreen(COL_BG);
  drawChatHeader();
  FONT_ASCII(d);
  d.setTextColor(chatDimColor(), COL_BG);
  d.setCursor(8, 24);
  d.print((title && title[0]) ? title : "TEXT");
  if (chatLooksAsciiArt(text)) drawChatStreamAsciiText(text);
  else drawChatStreamWrappedText(text);
}

static void drawChatCornerStatus(const char *status, uint16_t col = CHAT_DIM) {
  auto &d = M5Cardputer.Display;
  const char *text = (status && status[0]) ? status : "READY";
  FONT_ASCII(d);
  int w = d.textWidth(text);
  int x = CONTENT_W - w - 4;
  if (x < 0) x = 0;
  d.fillRect(x - 3, 116, w + 7, 15, COL_BG);
  d.setTextColor(col == CHAT_DIM ? chatAccentColor() : col, COL_BG);
  d.setCursor(x, 118);
  d.print(text);
}

static void drawChatStreamAsrText(const char *text) {
  drawChatStreamTextPage("YOU", text);
}

static void chatDrainBufferedAudio() {
  chatAudioStreaming = false;
  chatDeferPlayback = false;
  chatPcmStarted = true;
  speakerOn();
  while (chatPcmCount >= CHAT_PLAY_SAMPLES) {
    chatPcmPumpPlayback();
    M5Cardputer.update();
    delay(1);
  }
  while (M5Cardputer.Speaker.isPlaying(0)) {
    M5Cardputer.update();
    delay(5);
  }
  M5Cardputer.Speaker.stop();
}

static bool chatStartUplink(WiFiClient &client) {
  if (chatUplinkActive) return true;
  chatStopPlaybackTask();
  M5Cardputer.Speaker.stop();
  drawChatStreamHome("LISTEN", COL_GREEN);
  drawChatStreamStats();

  if (!prepareMicInput(true)) {
    drawChatStreamHome("MIC ERR", COL_RED);
    delay(800);
    return false;
  }

  if (!writeWsText(client, "{\"type\":\"uplink_start\",\"codec\":\"pcm_s16le\",\"sampleRate\":16000,\"channels\":1,\"chunkSamples\":256,\"phase\":3}")) {
    drawChatStreamHome("UP ERR", COL_RED);
    return false;
  }

  chatUplinkChunks = 0;
  chatUplinkStartedAt = millis();
  chatUplinkLevel = 0;
  chatUplinkDc = 0;
  chatUplinkHum = 0;
  chatUplinkLpf = 0;
  chatUplinkNoiseRms = 90;
  chatUplinkSoftGateQ8 = 256;
  chatUplinkActive = true;
  chatUplinkQueued = false;
  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  return true;
}

static bool chatPumpUplink(WiFiClient &client) {
  if (!chatUplinkActive || !client.connected()) return false;
  if (M5Cardputer.Mic.isRecording() > 0) return true;
  processMicBuffer(recBuf[0], REC_N, chatUplinkDc, chatUplinkHum, chatUplinkLpf, chatUplinkNoiseRms, chatUplinkSoftGateQ8);
  uint16_t peak = 0;
  for (size_t i = 0; i < REC_N; i++) {
    recBuf[0][i] = (int16_t)((int32_t)recBuf[0][i] * 3 / 4);
    uint16_t a = abs(recBuf[0][i]);
    if (a > peak) peak = a;
  }
  chatUplinkLevel = (chatUplinkLevel * 3 + peak) / 4;
  if (!writeWsBinary(client, (const uint8_t *)recBuf[0], REC_N * sizeof(int16_t))) {
    drawChatStreamHome("UP ERR", COL_RED);
    M5Cardputer.Mic.end();
    micInputReady = false;
    chatUplinkActive = false;
    return false;
  }
  chatUplinkChunks++;
  if ((chatUplinkChunks & 0x07) == 0) {
    drawChatListenMeter(millis() - chatUplinkStartedAt, chatUplinkLevel);
  }
  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  return true;
}

static void chatStopUplink(WiFiClient &client) {
  if (!chatUplinkActive) return;
  while (M5Cardputer.Mic.isRecording() > 0) {
    M5Cardputer.update();
    delay(1);
  }
  M5Cardputer.Mic.end();
  micInputReady = false;
  chatUplinkActive = false;
  chatUplinkQueued = false;
  writeWsText(client, "{\"type\":\"uplink_end\",\"phase\":3}");
  chatStartPlaybackTask();
  drawChatStreamHome("THINKING", COL_GREEN);
}

static void chatCancelUplink(WiFiClient &client) {
  if (!chatUplinkActive) return;
  M5Cardputer.Mic.end();
  micInputReady = false;
  chatUplinkActive = false;
  chatUplinkQueued = false;
  writeWsText(client, "{\"type\":\"uplink_cancel\",\"reason\":\"tap\"}");
  chatStartPlaybackTask();
  drawChatStreamHome("READY", COL_GREEN);
}

static void chatAbortUplinkLocal() {
  if (!chatUplinkActive) return;
  M5Cardputer.Mic.end();
  micInputReady = false;
  chatUplinkActive = false;
  chatUplinkQueued = false;
}

static void drawChatStreamHome(const char *status, uint16_t col = COL_GREEN) {
  auto &d = M5Cardputer.Display;
  chatTextPageVisible = false;
  d.fillScreen(COL_BG);
  drawChatHeader();
  FONT_ASCII(d);
  d.setTextColor(col == COL_RED ? COL_RED : chatAccentColor(), COL_BG);
  const char *text = (status && status[0]) ? status : "CONNECTING";
  int w = d.textWidth(text);
  d.setCursor((CONTENT_W - w) / 2, 44);
  d.print(text);
  if (strcmp(text, "READY") == 0) {
    drawChatHintLine("HOLD SPACE TO TALK", 76, chatDimColor());
    drawChatHintLine("RELEASE TO SEND", 94, chatDimColor());
  } else if (strcmp(text, "LISTEN") == 0) {
    drawChatListenMeter(0, 0);
  } else if (strcmp(text, "THINKING") == 0) {
    drawChatHintLine("ASR / LLM", 82, chatDimColor());
  } else if (strcmp(text, "SPEAKING") == 0) {
    drawChatHintLine("SPACE INTERRUPTS", 82, chatDimColor());
  } else if (col == COL_RED) {
    drawChatHintLine("PRESS C TO EXIT", 82, chatDimColor());
  }
}

static uint8_t chatStreamScreen() {
  waitRelease();
  uint32_t lastInputMs = millis();
  WiFiClient client;
  char detail[32] = {0};
  uint8_t wsFrame[1152];
  uint32_t lastStatsDraw = 0;
  bool chatSpaceHeld = false;
  uint32_t chatSpaceDownAt = 0;

  if (!chatPcmAlloc()) {
    drawChatStreamHome("NO RAM", COL_RED);
    delay(900);
    waitRelease();
    return APP_LAUNCHER;
  }
  drawChatStreamHome("CONNECTING", COL_GREEN);
  chatPcmReset();
  bool connected = false;
  if (!sdMount()) {
    strlcpy(detail, "NO SD", sizeof(detail));
  } else {
    connected = chatStreamConnectMounted(client, detail, sizeof(detail));
    SD.end();
  }
  drawChatStreamHome(connected ? "READY" : detail, connected ? COL_GREEN : COL_RED);
  if (connected) {
    chatStartPlaybackTask();
    drawChatStreamStats();
  }

  while (true) {
    M5Cardputer.update();
    if (connected && !client.connected()) {
      connected = false;
      client.stop();
      wifiPowerDown();
      strlcpy(detail, "DISCONNECTED", sizeof(detail));
      drawChatStreamHome(detail, COL_RED);
      chatAbortUplinkLocal();
      chatStopPlaybackTask();
      M5Cardputer.Speaker.stop();
      chatPcmFree();
      delay(900);
      waitRelease();
      return APP_LAUNCHER;
    }
    if (connected && client.available() >= 2) {
      size_t len = 0;
      uint8_t opcode = 0;
      if (readWsFrame(client, wsFrame, sizeof(wsFrame), len, opcode, 1200)) {
        if (opcode == 0x02 && len > 0) {
          if (!speakerOutputReady) speakerOn();
          chatAudioStreaming = true;
          chatBinaryFrames++;
          chatBinaryBytes += len;
          chatPcmPushBytes(wsFrame, len);
          chatPcmPumpPlayback();
          lastInputMs = millis();
        } else if (opcode == 0x01 && len > 0) {
          size_t textLen = len < sizeof(wsFrame) - 1 ? len : sizeof(wsFrame) - 1;
          wsFrame[textLen] = 0;
          if (strstr((const char *)wsFrame, "audio_start")) {
            chatTtsPlaying = strstr((const char *)wsFrame, "\"source\":\"tts\"") != nullptr;
            chatPcmReset(chatTextPageVisible && chatTtsPlaying);
            chatTtsPlaying = strstr((const char *)wsFrame, "\"source\":\"tts\"") != nullptr;
            if (chatTtsPlaying) {
              M5Cardputer.Mic.end();
              micInputReady = false;
              M5Cardputer.Speaker.stop();
              M5Cardputer.Speaker.end();
              speakerOutputReady = false;
              delay(8);
              speakerOn();
            }
            chatAudioStreaming = true;
            chatReadyAfterPlayback = false;
            chatStartPlaybackTask();
          }
          if (strstr((const char *)wsFrame, "audio_end")) {
            bool wasTts = chatTtsPlaying;
            chatAudioStreaming = false;
            if (wasTts) chatTunePlaybackStartBuffer();
            if (wasTts) {
              if (!chatPcmStarted && chatPcmAvailable() > 0) {
                chatPcmStarted = true;
                chatPcmPumpPlayback();
              }
              chatReadyAfterPlayback = true;
            } else {
              if (chatTextPageVisible) drawChatCornerStatus("READY", CHAT_DIM);
              else {
                drawChatStreamHome("READY", COL_GREEN);
                drawChatStreamStats();
              }
            }
            chatTtsPlaying = false;
          }
          if (strstr((const char *)wsFrame, "uplink_saved")) {
            drawChatStreamStats();
          }
          if (strstr((const char *)wsFrame, "listen_cancelled")) {
            drawChatStreamHome("READY", COL_GREEN);
          }
          if (strstr((const char *)wsFrame, "asr_start")) {
            if (!chatTextPageVisible) drawChatStreamStats();
          }
          if (strstr((const char *)wsFrame, "asr_text")) {
            char asrText[384];
            if (extractJsonStringValue(String((const char *)wsFrame), "text", asrText, sizeof(asrText))) {
              drawChatStreamAsrText(asrText);
              drawChatCornerStatus("THINKING", CHAT_DIM);
            } else if (!chatTextPageVisible) drawChatStreamStats();
          }
          if (strstr((const char *)wsFrame, "asr_error")) {
            drawChatStreamHome("ASR ERR", COL_RED);
          }
          if (strstr((const char *)wsFrame, "reply_start")) {
            if (chatTextPageVisible) drawChatCornerStatus("THINKING", CHAT_DIM);
            else drawChatStreamHome("THINKING", COL_GREEN);
          }
          if (strstr((const char *)wsFrame, "reply_text")) {
            char replyText[384];
            if (extractJsonStringValue(String((const char *)wsFrame), "text", replyText, sizeof(replyText))) {
              drawChatStreamTextPage("REPLY", replyText);
              drawChatCornerStatus("SPEAKING", CHAT_DIM);
            } else {
              drawChatStreamHome("REPLY OK", COL_GREEN);
            }
          }
          if (strstr((const char *)wsFrame, "tts_start")) {
            if (chatTextPageVisible) drawChatCornerStatus("SPEAKING", CHAT_DIM);
            else drawChatStreamHome("SPEAKING", COL_GREEN);
          }
          if (strstr((const char *)wsFrame, "tts_error")) {
            drawChatStreamHome("TTS ERR", COL_RED);
            chatReadyAfterPlayback = false;
          }
        } else if (opcode == 0x08) {
          connected = false;
          client.stop();
          wifiPowerDown();
          strlcpy(detail, "DISCONNECTED", sizeof(detail));
          drawChatStreamHome(detail, COL_RED);
          chatAbortUplinkLocal();
          chatStopPlaybackTask();
          M5Cardputer.Speaker.stop();
          chatPcmFree();
          delay(900);
          waitRelease();
          return APP_LAUNCHER;
        }
      }
    }
    chatPcmPumpPlayback();
    if (chatUplinkActive) chatPumpUplink(client);
    if (connected && millis() - lastStatsDraw >= 1000) {
      lastStatsDraw = millis();
      drawChatStreamStats();
    }
    bool spaceNow = keySpace();
    if (connected && spaceNow && !chatSpaceHeld) {
      lastInputMs = millis();
      chatSpaceHeld = true;
      chatSpaceDownAt = millis();
      if (!chatUplinkActive) {
        chatTextPageVisible = false;
        writeWsText(client, "{\"type\":\"interrupt\"}");
        M5Cardputer.Speaker.stop();
        chatPcmReset();
        chatAudioStreaming = false;
        chatTtsPlaying = false;
        chatStartUplink(client);
      }
    } else if (chatSpaceHeld && !spaceNow) {
      lastInputMs = millis();
      chatSpaceHeld = false;
      uint32_t heldMs = millis() - chatSpaceDownAt;
      if (chatUplinkActive) {
        if (heldMs < CHAT_TAP_CANCEL_MS || chatUplinkChunks < 3) chatCancelUplink(client);
        else chatStopUplink(client);
      }
    }
    if (chatUplinkActive && millis() - chatUplinkStartedAt > CHAT_MAX_UPLINK_MS) {
      chatSpaceHeld = false;
      chatStopUplink(client);
    }
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyEsc()) {
        chatStopUplink(client);
        chatStopPlaybackTask();
        M5Cardputer.Speaker.stop();
        chatPcmFree();
        client.stop();
        wifiPowerDown();
        waitRelease();
        return APP_SLEEP;
      }
      if (keyChat()) {
        waitRelease();
      }
      if (keyBrightUp() || keyBrightDn()) {
        size_t n = sizeof(CHAT_ACCENT_COLORS) / sizeof(CHAT_ACCENT_COLORS[0]);
        if (keyBrightUp()) chatAccentIndex = (chatAccentIndex + 1) % n;
        else chatAccentIndex = (chatAccentIndex + n - 1) % n;
        drawChatStreamHome("COLOR", COL_GREEN);
        waitRelease();
      }
    }
    if (chatReadyAfterPlayback && !chatAudioStreaming &&
        chatPcmAvailable() < CHAT_PLAY_SAMPLES &&
        M5Cardputer.Speaker.isPlaying(0) == 0) {
      chatReadyAfterPlayback = false;
      M5Cardputer.Speaker.stop();
      if (chatTextPageVisible) drawChatCornerStatus("READY", CHAT_DIM);
      else drawChatStreamHome("READY", COL_GREEN);
    }
    systemIdleTick();
    if (autoSleepDue(lastInputMs)) {
      chatStopUplink(client);
      chatStopPlaybackTask();
      M5Cardputer.Speaker.stop();
      chatPcmFree();
      client.stop();
      wifiPowerDown();
      return APP_SLEEP;
    }
    delay(2);
  }
}

static uint8_t chatHttpScreen() {
  bool redraw = true;
  uint32_t lastInputMs = millis();
  char reply[180] = {0};
  char audioUrl[256] = {0};
  waitRelease();

  while (true) {
    M5Cardputer.update();
    if (redraw) {
      drawChatHome("回车开始说话", reply);
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyEsc()) { waitRelease(); return APP_LAUNCHER; }
      if (keySpace()) { waitRelease(); return APP_REC_RECORD; }
      if (keyEnter()) {
        if (!sdMount()) {
          showMsg("CHAT", "未检测到 SD 卡", COL_RED);
          waitRelease();
          redraw = true;
          continue;
        }
        bool ok = chatRecordOnceMounted(CHAT_LAST_PATH);
        SD.end();
        if (!ok) {
          showMsg("CHAT", "录音取消或失败", COL_RED);
          waitRelease();
          redraw = true;
          continue;
        }
        drawChatHome("正在上传并识别...", nullptr);
        if (!sdMount()) {
          showMsg("CHAT", "SD 读取失败", COL_RED);
          waitRelease();
          redraw = true;
          continue;
        }
        audioUrl[0] = 0;
        uint8_t status = uploadChatWavMounted(CHAT_LAST_PATH, reply, sizeof(reply), audioUrl, sizeof(audioUrl));
        SD.end();
        if (status != UPSTAT_DONE) {
          snprintf(reply, sizeof(reply), "%s", uploadStatusLabel(status));
          showMsg("CHAT", uploadStatusLabel(status), COL_RED);
        } else if (!reply[0]) {
          snprintf(reply, sizeof(reply), "已收到, 但没有返回文字");
        } else if (audioUrl[0] && sdMount()) {
          drawChatHome(reply, "正在播放语音...");
          if (downloadHttpToFileMounted(audioUrl, CHAT_REPLY_PATH)) {
            playWavQuickMounted(CHAT_REPLY_PATH);
          }
          SD.end();
        }
        waitRelease();
        redraw = true;
      } else if (keyChat()) {
        waitRelease();
        redraw = true;
      }
    }
    systemIdleTick();
    if (autoSleepDue(lastInputMs)) return APP_SLEEP;
    delay(8);
  }
}

static uint8_t pomodoroScreen() {
  bool running = false;
  uint32_t remainSec = 25 * 60;
  uint32_t lastTick = millis();
  uint32_t lastInputMs = millis();
  bool redraw = true;
  waitRelease();

  while (true) {
    M5Cardputer.update();
    uint32_t now = millis();
    if (running && now - lastTick >= 1000) {
      uint32_t ticks = (now - lastTick) / 1000;
      lastTick += ticks * 1000;
      if (remainSec > ticks) remainSec -= ticks;
      else { remainSec = 0; running = false; }
      redraw = true;
    }

    if (redraw) {
      auto &d = M5Cardputer.Display;
      char buf[8];
      snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(remainSec / 60), (unsigned long)(remainSec % 60));
      d.fillScreen(COL_BG);
      drawHeader("番茄钟");
      FONT_TIMER(d);
      d.setTextColor(remainSec == 0 ? COL_RED : COL_GREEN, COL_BG);
      d.setCursor(40, 52);
      d.print(buf);
      FONT_CN_12(d);
      d.setTextColor(running ? COL_GREEN : COL_DIM, COL_BG);
      d.setCursor(76, 88);
      d.print(running ? "工作中" : (remainSec == 0 ? "已完成" : "暂停"));
      drawFooter("回车开始/暂停  Del重置  Esc返回");
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyEsc()) { waitRelease(); return APP_LAUNCHER; }
      if (keySpace()) { waitRelease(); return APP_REC_RECORD; }
      if (keyEnter()) {
        if (remainSec == 0) remainSec = 25 * 60;
        running = !running;
        lastTick = millis();
        redraw = true;
        waitRelease();
      } else if (keyDel()) {
        running = false;
        remainSec = 25 * 60;
        redraw = true;
        waitRelease();
      }
    }
    systemIdleTick();
    if (!running && autoSleepDue(lastInputMs)) return APP_SLEEP;
    delay(8);
  }
}

static void runApp(uint8_t app) {
  while (true) {
    if (app == APP_SLEEP) return;
    if (app == APP_REC_RECORD) {
      int n = recordingScreen();
      afterRecordingFlow(n);
      return;
    }
    if (app == APP_REC_LIST) {
      listFlow(0);
      return;
    }
    if (app == APP_POMODORO) {
      app = pomodoroScreen();
      continue;
    }
    if (app == APP_CHAT) {
      app = chatStreamScreen();
      continue;
    }
    if (app == APP_WIFI) {
      app = wifiSettingsScreen();
      continue;
    }
    if (app == APP_LAUNCHER) {
      uint8_t next = launcherScreen();
      if (next == APP_LAUNCHER) return;
      app = next;
      continue;
    }
    return;
  }
}

// ---------- 麦克风预热(开机 / 唤醒后调用): 空跑消耗冷启动不稳定, 之后保持常开 ----------
static void micWarmup(bool rearmAfterWarmup = true, int warmBuffers = 24) {
  // 开机/唤醒的第一条录音也必须走完整输入链路配置; 否则首次录音增益会偏低。
  micInputReady = false;
  if (!prepareMicInput()) return;
  forceMicRearm = rearmAfterWarmup;
  static int16_t warm[REC_N];
  for (int i = 0; i < warmBuffers; i++) {
    M5Cardputer.Mic.record(warm, REC_N, REC_RATE);
    while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  }
  // 不 Mic.end(): 保持常开
}

// ---------- 息屏(轻睡眠): 关背光; 一直睡到"键盘中断/Go键"才醒; 空格=录音, 回车=列表, C=CHAT, F=番茄钟, Go=应用列表 ----------
// 核心思路: 不调 Mic.end() → ES8311 模拟段保持通电, 没有掉电瞬态, 没有爆音.
// CONFIG_PM_ENABLE 未启用: I2S 不持有阻止轻睡眠的电源锁, 可直接 esp_light_sleep_start().
// I2S 任务在睡眠期间被 RTOS 暂停, APB 时钟门控; 唤醒后自动续跑.
static void goSleep() {
  auto &d = M5Cardputer.Display;

  if (micInputReady) {
    while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    M5Cardputer.Mic.end();
    micInputReady = false;
    forceMicRearm = true;
    const uint8_t ES = 0x18;
    M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
    M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
    delay(8);
  }

  // 静音 ES8311 输出路径(不掉电 0x0D/0x12): 防止睡眠瞬态被无 mute 脚功放放大
  const uint8_t ES = 0x18;
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(0);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0x00, 400000);  // 先断开 DAC->HP mixer
  delay(2);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x00, 400000);  // 关 HP drive
  delay(5);
  M5Cardputer.Speaker.end();
  speakerOutputReady = false;

  d.fillScreen(COL_BG);
  d.setBrightness(0);          // 关背光 = 最大省电点
  delay(10);
  bool cleanAborted = false;
  processPendingFrictionQueue(cleanAborted);
  if (cleanAborted) {
    applyBrightness();
    M5Cardputer.update();
    uint8_t app = APP_REC_LIST;
    wakeAppFromPressedKeys(app);
    wakeApp = app;
    autoRecordPending = true;
    waitRelease();
    return;
  }

#if UPLOAD_WIFI_ENABLED
  if (!g_mediaBusy) runUploadBatchMounted(false);
#endif

  // 打开键盘芯片(TCA8418 @0x34)的按键中断 -> 按键时拉低 GPIO11; Go/BtnA 是 GPIO0 低有效
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x01, 400000);   // CFG: KE_IEN=1
  gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  while (true) {
    M5Cardputer.update();                                          // 排空键盘事件
    M5Cardputer.In_I2C.writeRegister8(0x34, 0x02, 0x03, 400000);  // 清中断标志 -> INT 线复位为高
    esp_light_sleep_start();                                       // 一直睡到 GPIO11 或 GPIO0 变低
    bool recognized = false;
    for (int i = 0; i < 10; i++) {
      delay(8);
      M5Cardputer.update();
      uint8_t app = APP_REC_RECORD;
      if ((M5Cardputer.Keyboard.isPressed() || keyGo()) && wakeAppFromPressedKeys(app)) {
        wakeApp = app;
        recognized = true;
        break;
      }
    }
    if (recognized) break;
  }

  // 唤醒: 关掉唤醒源 + 键盘中断
  gpio_wakeup_disable(GPIO_NUM_11);
  gpio_wakeup_disable(GPIO_NUM_0);
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x00, 400000);
  applyBrightness();
  applyBrightness();
  if (wakeApp == APP_REC_RECORD) micWarmup(false, 16);  // 息屏唤醒复用预热链路, 减少开始爆音和等待
  autoRecordPending = true;
  waitRelease();
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // 开机不自动开扬声器(消除开机功放上电"爆音"), 播放时再手动开
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(SCREEN_ROT);
  applyBrightness();
  M5Cardputer.Display.setTextWrap(false);   // 关闭自动换行: 过长文字右侧截断, 不会换行掉出屏幕
#if UPLOAD_WIFI_ENABLED
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
#endif
  {                          // internal_spk=false 跳过扬声器配置, 手动补上 Adv 扬声器引脚
    auto sc = M5Cardputer.Speaker.config();
    sc.pin_bck = 41; sc.pin_ws = 43; sc.pin_data_out = 42;
    sc.i2s_port = I2S_NUM_1; sc.magnification = 16;
    M5Cardputer.Speaker.config(sc);
  }

  int p;
  p = M5.getPin(m5::pin_name_t::sd_spi_sclk); if (p >= 0) sdSCLK = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_cipo); if (p >= 0) sdMISO = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_copi); if (p >= 0) sdMOSI = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_cs);   if (p >= 0) sdCS   = p;

  micWarmup(true, 8);
  loadHotkeys();
  hotkeysLoaded = true;
  scanRecordings();

  M5Cardputer.Display.fillScreen(COL_BG);
}

void loop() {
  M5Cardputer.update();

  if (autoRecordPending) {
    autoRecordPending = false;
    uint8_t app = wakeApp;
    wakeApp = APP_REC_RECORD;
    runApp(app);
    goSleep();
    return;
  }

  goSleep();
}
