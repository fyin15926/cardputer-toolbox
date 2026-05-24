/*
 * 我的工具箱 (My Toolbox) for M5Stack Cardputer-Adv
 * 风格: 纯黑背景 + 黑客帝国荧光绿 + 线性 + 中文界面 + 横屏(240x135)
 *
 * 录音应用交互:
 *   开机: 自动开始录音; 息屏: 空格=录音, 回车=列表
 *   录制中: 空格=暂停/继续; 回车=结束(存盘并进入列表); Esc=存盘并息屏; 长按Del 1.2秒=取消并删除本条; W/S=音量
 *   录音列表: 左/右切 REC/KEY/IMP; ;/. 上下选; 回车=播放; Esc=息屏; 长按Del=删除; Ctrl+键=绑定快捷; Ctrl+Enter=标重要; Alt=降噪
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

// ---------- 屏幕方向 ----------
// 横屏: 1 或 3. 若上下颠倒, 改另一个即可.
#define SCREEN_ROT 1

// ---------- 配色 ----------
#define COL_BG    0x0000   // 纯黑
#define COL_GREEN 0x07E0   // 荧光绿(亮)
#define COL_DIM   0x0320   // 暗绿
#define COL_RED   0xF800   // 录音红点
#define COL_WHITE 0xFFFF   // 播放头白线

// ---------- 全屏 UI 布局常量 ----------
// 屏幕 240×135; 不再绘制右侧标签栏, 所有界面使用全宽内容区
#define CONTENT_W  240     // 内容区宽度 (x: 0..239)
#define WAVE_TOP    20     // 波形区顶 y
#define WAVE_BOT   109     // 波形区底 y  (余 26px 给计时器)
#define WAVE_H      89     // 波形区高度
#define WAVE_CY     50     // A/B 分界: 约 2:1, B线作为主视觉
static const int A_CY   = (WAVE_TOP + WAVE_CY) / 2;        // A线/轨道线中轴
static const int A_HALF = (WAVE_CY - WAVE_TOP) / 2 - 2;    // A线最大半幅
static const int B_CY   = (WAVE_CY + WAVE_BOT) / 2;        // B线/监听线中轴
static const int B_HALF = (WAVE_BOT - WAVE_CY) / 2 - 2;    // B线最大半幅
static const uint8_t REC_B_SMOOTH = 3;   // 录音 B线平滑: 越大越稳, 越小越灵敏
static const uint8_t PB_B_SMOOTH  = 4;   // 播放 B线平滑
static const uint8_t B_DECAY      = 5;   // 无新音频时回中线速度: 越小回落越快
static const int32_t A_RMS_FULL   = 24000; // A线满格阈值: 越大越不敏感

// ---------- 录音参数 ----------
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
static const size_t   PB_N     = 512;    // 播放缓冲样本数 (~32ms, B线更顺)
static int16_t recBuf[2][REC_N];
static int16_t pbBuf[2][PB_N];           // 播放双缓冲(放一块/读另一块, 避免覆盖破音)
static int16_t seekToneBuf[384];
int recGain = 50;                        // 录音软件增益默认值(W/S 现场可调, 带削波保护)
int playVol = 200;                       // 回放音量(+/- 可调, 0..255)

// SD 引脚(运行时由 M5Unified 按机型给出, 失败则回退到 Adv 已知值)
int sdSCLK = 40, sdMISO = 39, sdMOSI = 14, sdCS = 12;

static const char *REC_DIR = "/REC";
static const char *SHORTCUT_DIR = "/SHORTCUT";
static const char *IMPORTANT_DIR = "/IMPORTANT";
static const char *HOTKEY_PATH = "/SHORTCUT/keys.txt";
static const char *OLD_IMPORTANT_HOTKEY_PATH = "/IMPORTANT/keys.txt";
static const char *OLD_HOTKEY_PATH = "/REC/keys.txt";
static const char *NEXT_INDEX_PATH = "/REC/.next";
static const int MAX_REC = 9999;
static const uint16_t FRICTION_NOW_SEC = 60;
static const uint16_t FRICTION_IDLE_MAX_SEC = 20 * 60;

enum RecKind : uint8_t { REC_NORMAL = 0, REC_SHORTCUT = 1, REC_IMPORTANT = 2 };

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
static int wakeAction = 1;  // R_RECORD, 这里早于返回码宏定义
static bool speakerOutputReady = false;

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
int g_listReturnRec = 0;     // 播放页返回列表时应重新选中的录音
int g_carryDeleteRec = 0;
uint32_t g_carryDeleteStart = 0;
uint8_t g_listMode = REC_NORMAL;  // 0普通 / 1快捷 / 2重要
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
static bool keySpace() { return M5Cardputer.Keyboard.isKeyPressed(' '); }
static bool keyEsc()   { return M5Cardputer.Keyboard.isKeyPressed('`'); }  // Cardputer 物理 Esc 键映射为左上角 `
static bool keyDel()   { return M5Cardputer.Keyboard.keysState().del; }
static bool keyEnter() { return M5Cardputer.Keyboard.keysState().enter; }
static bool keyTab()   { return M5Cardputer.Keyboard.keysState().tab; }
static bool keyUp()    { return M5Cardputer.Keyboard.isKeyPressed(';'); }
static bool keyDown()  { return M5Cardputer.Keyboard.isKeyPressed('.'); }
static bool keyLeft()  { return M5Cardputer.Keyboard.isKeyPressed(','); }
static bool keyRight() { return M5Cardputer.Keyboard.isKeyPressed('/'); }
static bool keyVolUp() { return M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+'); }
static bool keyVolDn() { return M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_'); }

static void adjustPlayVolume(int delta) {
  playVol = max(0, min(255, playVol + delta));
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(playVol);
}

// 等所有键松开
static void waitRelease() {
  do { M5Cardputer.update(); delay(8); } while (M5Cardputer.Keyboard.isPressed());
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

static void processMicBuffer(int16_t *buf, size_t n, int32_t &lpf) {
  for (size_t k = 0; k < n; k++) {
    int32_t x = (int32_t)buf[k] * recGain;
    lpf += (int32_t)(((int64_t)(x - lpf) * 160) >> 8);
    float yf = 32767.0f * tanhf((float)lpf / 32767.0f);
    buf[k] = (int16_t)yf;
  }
}

static int8_t calcTrackAmp(const int16_t *buf, size_t n) {
  if (!buf || n == 0) return 0;
  int64_t sumSq = 0;
  for (size_t k = 0; k < n; k++) sumSq += (int64_t)buf[k] * buf[k];
  float rms = sqrtf((float)((double)sumSq / n));
  int sign = (buf[n / 2] >= 0) ? 1 : -1;
  int amp = (int)(rms * A_HALF / A_RMS_FULL) * sign;
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

#define DRAW_STATUS_BATTERY(g) do { \
  int bat = M5.Power.getBatteryLevel(); \
  uint16_t col = (bat <= 20) ? COL_RED : COL_GREEN; \
  (g).fillRect(CONTENT_W - 46, 0, 46, 18, COL_BG); \
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
static void drawFooter(const char *hint) {
  auto &d = M5Cardputer.Display;
  int y = d.height() - 15;
  d.drawFastHLine(0, y - 3, d.width(), COL_DIM);
  d.fillRect(0, y, d.width(), 15, COL_BG);
  FONT_CN_12(d);
  d.setTextColor(COL_DIM, COL_BG);
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
  const int x = 52, y = 116, w = 136, h = 18;
  const int barX = x + 38, barY = y + 6, barW = 88, barH = 7;
  int fillW = (int)((uint32_t)barW * playVol / 255);
  d.fillRect(x - 2, y - 1, w + 4, h + 2, COL_BG);
  d.drawRoundRect(x, y, w, h, 3, col);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(x + 7, y + 2);
  d.print("VOL");
  d.drawRect(barX, barY, barW, barH, col);
  if (fillW > 0) d.fillRect(barX + 1, barY + 1, max(0, fillW - 2), barH - 2, col);
}

static void drawCanvasVolumeToast(M5Canvas &cv, uint16_t col = COL_GREEN) {
  const int x = 52, y = 116, w = 136, h = 18;
  const int barX = x + 38, barY = y + 6, barW = 88, barH = 7;
  int fillW = (int)((uint32_t)barW * playVol / 255);
  cv.fillRect(x - 2, y - 1, w + 4, h + 2, COL_BG);
  cv.drawRoundRect(x, y, w, h, 3, col);
  FONT_ASCII(cv);
  cv.setTextColor(col, COL_BG);
  cv.setCursor(x + 7, y + 2);
  cv.print("VOL");
  cv.drawRect(barX, barY, barW, barH, col);
  if (fillW > 0) cv.fillRect(barX + 1, barY + 1, max(0, fillW - 2), barH - 2, col);
  cv.pushSprite(0, 0);
}

static void drawCanvasSaveBadge(M5Canvas &cv) {
  const int x = CONTENT_W - 62;
  const int y = 112;
  const int w = 58;
  const int h = 19;
  cv.fillRoundRect(x, y, w, h, 4, COL_BG);
  cv.drawRoundRect(x, y, w, h, 4, COL_DIM);
  cv.drawFastHLine(x + 5, y, w - 10, COL_GREEN);
  cv.drawFastVLine(x, y + 5, h - 10, COL_GREEN);
  cv.drawPixel(x + w - 2, y + 2, COL_GREEN);
  cv.drawPixel(x + w - 3, y + h - 3, COL_DIM);
  cv.fillRect(x + 8, y + 5, 8, 8, COL_GREEN);
  cv.fillRect(x + 10, y + 6, 4, 2, COL_BG);
  cv.drawFastHLine(x + 9, y + 15, 7, COL_GREEN);
  FONT_ASCII(cv);
  cv.setTextColor(COL_GREEN, COL_BG);
  cv.setCursor(x + 21, y + 2);
  cv.print("SAVE");
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

// 当前是否按下了一个"已绑定的播放键"(非 Ctrl): 返回对应录音编号, 否则 -1
static int pressedHotkeyRec() {
  if (keyCtrl()) return -1;          // Ctrl+键 是"绑定", 不是"播放"
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

static uint16_t durationFromFileSize(uint32_t fileSize) {
  if (fileSize <= 44) return 0;
  uint32_t sec = (((fileSize - 44) / 2) + REC_RATE - 1) / REC_RATE;
  return (sec > 65535) ? 65535 : (uint16_t)sec;
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
      if (n > 0 && !e.isDirectory() && e.size() > 44) {
        if (n > maxIdx) maxIdx = n;
        insertRecListSorted(n, kind);
        setRecDuration(n, e.size());
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
  int maxIdx = 0;
  recCount = 0;
  clearImportantBits();
  scanRecordingDir(REC_DIR, REC_NORMAL, maxIdx);
  scanRecordingDir(SHORTCUT_DIR, REC_SHORTCUT, maxIdx);
  scanRecordingDir(IMPORTANT_DIR, REC_IMPORTANT, maxIdx);

  int idx = lowestAvailableRecordingIndex();
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

static void drawTrackBars(M5Canvas &cv) {
  for (int i = 0; i < WAVE_BARS; i++) {
    if (waveBarCounts[i] == 0) continue;
    drawTrackBar(cv, i, waveBars[i]);
  }
}

static void updateLiveWaveVisual(int8_t *dst, const int16_t *src, size_t n, int half, uint8_t smooth, uint8_t decay) {
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
  for (int x = 0; x < CONTENT_W; x++) {
    int idx = (int)((uint32_t)x * n / CONTENT_W);
    if (idx >= (int)n) idx = n - 1;
    int target = (int)((int32_t)src[idx] * 3 * half / 32767);
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

// 播放画面: A线=轨道线/Timeline A, B线=监听线/Monitor B(播放缓冲)
static void drawPlaybackCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH, B_DECAY);

  // --- 波形区 ---
  cv.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);

  // A线: 时间轴波形进度。录制页和播放页共用同款小波形柱。
  cv.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(cv);

  // B线: 当前播放缓冲实时跳动
  cv.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  drawLiveWaveVisual(cv, playbackLiveWave, B_CY);

  // 播放头白线(只穿过A线区域)
  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  cv.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  // --- 计时器 (底部左侧) ---
  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  cv.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  cv.setTextColor(COL_GREEN, COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(cv, 4, WAVE_BOT + 2, curBuf, COL_GREEN);
  if (deleteHeldMs > 0) {
    drawDeleteProgress(cv, deleteHeldMs);
  } else {
    cv.setTextColor(COL_DIM, COL_BG);
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(cv, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, COL_DIM);
  }
}

static void drawPlaybackAction(M5Canvas &cv, const char *msg, uint16_t col = COL_GREEN) {
  (void)cv;
  (void)msg;
  (void)col;
}

// ---------- 回放界面: 播放完自动回列表, 回车暂停/继续, 退格回列表, ;/.切换, +/- 音量, 长按Del删除, 空格去录音 ----------
// 返回 R_BACK(返回上一层), R_LIST(回列表), R_RECORD(去录音) 或 R_DELETE(删除当前录音)
int playbackScreen(const char *path, int recNum, int prevRec, int nextRec) {
  auto &d = M5Cardputer.Display;
  if (!sdMount()) { showMsg("播放", "SD 读取失败", COL_RED); return R_LIST; }
  File f = SD.open(path, FILE_READ);
  if (!f) { SD.end(); showMsg("播放", "打不开文件", COL_RED); return R_LIST; }
  uint32_t total = f.size();
  if (total <= 44) { f.close(); SD.end(); showMsg("播放", "文件为空", COL_RED); return R_LIST; }
  uint8_t hb[4]; f.seek(40); f.read(hb, 4);   // 按 WAV 头声明长度播放(尊重掐尾)
  uint32_t dataSize = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  uint32_t fileData = total - 44;
  if (dataSize == 0 || dataSize > fileData) dataSize = fileData;
  f.seek(44);
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(playbackLiveWave, 0, sizeof(playbackLiveWave));

  M5Canvas cv(&d);
  cv.createSprite(CONTENT_W, d.height());

  // 静态部分(画一次): 顶栏
  auto drawStatic = [&]() {
    cv.fillScreen(COL_BG);
    // 文件编号 (左上小字)
    char title[16];
    snprintf(title, sizeof(title), "REC_%04d", recNum);
    char hk = hotkeyOf(recNum);
    drawStatusTitleNoLine(cv, title, hk ? COL_DIM : COL_GREEN);
    if (hk) {
      char keyLabel[8];
      snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
      drawDseg14Text(cv, dseg14TextWidth(title) + 10, 1, keyLabel, COL_GREEN);
    }
    cv.drawFastHLine(0, WAVE_BOT + 1, CONTENT_W, 0x0820);
  };
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;
  bool paused = false;
  auto deleteHeldMs = [&]() -> uint32_t {
    if (delHoldStart == 0) return 0;
    uint32_t held = millis() - delHoldStart;
    if (held < DELETE_HINT_MS) return 0;
    return held > DELETE_HOLD_MS ? DELETE_HOLD_MS : held;
  };
  auto drawProgress = [&](uint32_t played, int16_t *liveWave = nullptr, size_t liveN = 0) {
    drawPlaybackCanvas(cv, played, dataSize, liveWave, liveN, paused, deleteHeldMs());
    cv.pushSprite(0, 0);
  };

  bool stop = false, playDone = false;
  int ret = R_BACK;
  uint32_t played = 0, remaining = dataSize;
  uint32_t lastSeek = 0;
  uint32_t lastPreviewDraw = 0;
  uint32_t fadeBytesLeft = REC_RATE * 2 * 80 / 1000;
  uint8_t previewBar = 0;
  int pi = 0;

  drawStatic();
  drawProgress(0);
  speakerOn();
  bool ignoreKeysUntilRelease = M5Cardputer.Keyboard.isPressed();

  while (!stop) {
    M5Cardputer.update();
    if (ignoreKeysUntilRelease) {
      if (!M5Cardputer.Keyboard.isPressed()) ignoreKeysUntilRelease = false;
    } else {
      if (keyDel()) {
        if (delHoldStart == 0) { delHoldStart = millis(); lastDelDraw = 0; }
        uint32_t held = millis() - delHoldStart;
        if (held >= DELETE_HOLD_MS) { ret = R_DELETE; stop = true; }
        else if (held >= DELETE_HINT_MS && millis() - lastDelDraw > 60) { lastDelDraw = millis(); drawProgress(played); }
      } else if (delHoldStart != 0) {
        delHoldStart = 0;
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
        else if (keyEnter()) {
          paused = !paused;
          if (paused) M5Cardputer.Speaker.stop();
          drawProgress(played);
          drawPlaybackAction(cv, paused ? "PAUSE" : "PLAY", paused ? COL_DIM : COL_GREEN);
          waitRelease();
        }
        else if (keyVolUp()) { adjustPlayVolume(25); drawCanvasVolumeToast(cv, COL_GREEN); }
        else if (keyVolDn()) { adjustPlayVolume(-25); drawCanvasVolumeToast(cv, COL_DIM); }
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
      fadeBytesLeft = REC_RATE * 2 * 80 / 1000;
      drawProgress(played);
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
      previewPlaybackWaveStep(f, dataSize, previewBar, 4);
      if (millis() - lastPreviewDraw > 70) {
        lastPreviewDraw = millis();
        drawProgress(played);
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
    drawProgress(played, liveWave, liveN);
  }
  cv.deleteSprite();
  M5Cardputer.Speaker.stop();
  f.close();
  SD.end();
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

// 录制. 开机自动进入; 调用此函数后创建文件并开始写入
int recordingScreen() {
  auto &d = M5Cardputer.Display;
  g_afterRecord = R_LIST;

  // 准备阶段仍显示 READY, 真正开始采样后才亮 REC 红点
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(recLiveWave, 0, sizeof(recLiveWave));
  M5Canvas cv(&d);
  cv.createSprite(CONTENT_W, d.height());
  drawRecCanvas(cv, 0, false, nullptr, true);
  cv.pushSprite(0, 0);

  if (!sdMount()) { cv.deleteSprite(); showMsg("录音机", "未检测到 SD 卡", COL_RED); return 0; }
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  int idx = nextRecordingIndex();
  char path[40];
  recordingPathKind(idx, REC_NORMAL, path, sizeof(path));
  File f = SD.open(path, FILE_WRITE);
  if (!f) { cv.deleteSprite(); SD.end(); showMsg("录音机", "无法写入文件", COL_RED); return 0; }
  nextRecHint = (idx < 9999) ? idx + 1 : 9999;
  writeWavHeader(f, REC_RATE, 0);

  bool mustRearm = forceMicRearm;
  bool micWasReady = micInputReady && !mustRearm;
  if (!prepareMicInput(mustRearm)) { cv.deleteSprite(); f.close(); SD.end(); showMsg("录音机", "麦克风启动失败", COL_RED); return 0; }
  if (!micWasReady) delay(20);
  if (mustRearm) {
    for (int i = 0; i < 12; i++) {
      M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
      while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    }
    forceMicRearm = false;
  }

  uint32_t dataBytes = 0;
  int b = 0;
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
  queueRecordBuffers();
  drawRecCanvas(cv, 0, true, nullptr);
  cv.pushSprite(0, 0);
  bool stop = false;
  bool cancelRec = false;
  bool recScreenOff = false;
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;
  bool ignoreStartKey = M5Cardputer.Keyboard.isPressed();
  int32_t lpf = 0;
  uint32_t skipBuffers = micWasReady ? 4 : 8;   // 准备阶段已热机, 只短掐头防按键声

  while (!stop) {
    while (true) {
      M5Cardputer.update();
      if (ignoreStartKey) {
        if (!M5Cardputer.Keyboard.isPressed()) ignoreStartKey = false;
      } else if (recScreenOff) {
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
          if (keyTab() || keySpace() || keyDel() || keyEnter()) {
            recScreenOff = false;
            ignoreStartKey = true;
            delHoldStart = 0;
            lastDelDraw = 0;
            M5Cardputer.Display.setBrightness(120);
            drawRecCanvas(cv, activeElapsed(), true, nullptr, false, paused);
            cv.pushSprite(0, 0);
          }
        }
      } else if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyTab()) {
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
            drawRecCanvas(cv, activeElapsed(), true, nullptr);
            cv.pushSprite(0, 0);
          } else {
            paused = true;
            pauseStart = millis();
            while (M5Cardputer.Mic.isRecording() > 0) { M5Cardputer.update(); delay(1); }
            drawRecCanvas(cv, activeElapsed(), false, nullptr, false, true);
            cv.pushSprite(0, 0);
          }
          waitRelease();
        }
        else if (keyEsc()) { g_afterRecord = R_BACK; stop = true; break; }
        else if (keyEnter()) { drawCanvasSaveBadge(cv); g_afterRecord = R_LIST; stop = true; break; }
        else if (keyAlt()) { drawCanvasToast(cv, "WAIT", COL_GREEN); g_afterRecord = R_NOISE; stop = true; break; }
        else if (keyVolUp()) { adjustPlayVolume(25); drawCanvasVolumeToast(cv, COL_GREEN); waitRelease(); }
        else if (keyVolDn()) { adjustPlayVolume(-25); drawCanvasVolumeToast(cv, COL_DIM); waitRelease(); }
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
            drawRecCanvas(cv, activeElapsed(), false, nullptr, false, paused, held);
            cv.pushSprite(0, 0);
          }
        } else if (delHoldStart != 0) {
          delHoldStart = 0;
          drawRecCanvas(cv, activeElapsed(), false, nullptr, false, paused);
          cv.pushSprite(0, 0);
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
    processMicBuffer(filled, REC_N, lpf);
    if (skipBuffers > 0) skipBuffers--;
    else { f.write((uint8_t *)filled, REC_N * sizeof(int16_t)); dataBytes += REC_N * sizeof(int16_t); }
    uint32_t now = millis();
    if (now - lastDraw >= 16) {
      lastDraw = now;
      bool blink = ((now / 400) % 2) == 0;
      uint32_t elapsed = activeElapsed();
      pushTrackBar(calcTrackAmp(filled, REC_N));
      uint32_t held = delHoldStart ? millis() - delHoldStart : 0;
      if (!recScreenOff) {
        drawRecCanvas(cv, elapsed, blink, filled, false, false, held);
        cv.pushSprite(0, 0);
      }
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
  }
  if (recScreenOff) M5Cardputer.Display.setBrightness(120);
  cv.deleteSprite();
  // 掐尾 ~0.3s.
  uint32_t tailTrim = REC_RATE * 6 / 10;
  uint32_t effData = (dataBytes > tailTrim) ? (dataBytes - tailTrim) : 0;
  if (!cancelRec && effData < REC_RATE) cancelRec = true;
  if (!cancelRec) writeWavHeader(f, REC_RATE, effData);
  f.close();
  if (!cancelRec && effData > 0) {
    setRecDuration(idx, 44 + effData);
    uint16_t dur = getRecDuration(idx);
    if (dur <= FRICTION_NOW_SEC) {
      if (suppressKeyFriction(path, effData)) setFrictionDone(idx, true);
    } else if (dur <= FRICTION_IDLE_MAX_SEC) {
      setFrictionPending(idx, true);
    }
  }
  if (cancelRec) {
    SD.remove(path);
    nextRecHint = idx;
    writeNextIndexCache(idx);
  }
  SD.end();
  while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  if (g_afterRecord != R_BACK) {
    micInputReady = false;
    M5Cardputer.Mic.end();
    const uint8_t ES = 0x18;
    M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
    M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  }
  if (cancelRec) return 0;
  insertRecListSorted(idx);
  nextRecHint = (idx < 9999) ? idx + 1 : 9999;
  writeNextIndexCache(nextRecHint);
  return idx;
}

// ---------- 录音列表: ;/.选, 回车放, Ctrl+键绑定, Alt降噪, 空格录音, Esc退出, 长按Del删除 ----------
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
  SD.end();
  setShortcutRec(recNum, false);
  setImportantRec(recNum, false);
  setFrictionDone(recNum, false);
  setFrictionPending(recNum, false);
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
      if (SD.remove(p)) deleted++;
    }
    dir.close();
  }
  SD.end();
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
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(COL_GREEN); waitRelease(); continue; }
        if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(COL_DIM); waitRelease(); continue; }
        if (keySpace()) return R_RECORD;
        if (keyEsc())   return R_BACK;
        if (keyDel())   return R_BACK;
      }
      delay(8);
    }
  }

  int sel = recCount - 1;   // 默认选最新录音(列表末尾)
  if (selectIdx > 0) {
    for (int i = 0; i < recCount; i++) {
      if (recList[i] == selectIdx) {
        sel = i;
        g_listMode = recKindOf(selectIdx);
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
      const int keyX = 126;
      const int durX = CONTENT_W - 72;
  int visRows = (d.height() - top - 18) / rowH;   // 底部留 18px 给提示
  if (visRows < 1) visRows = 1;

  if (!(g_carryDeleteRec > 0 && keyDel())) waitRelease();
  while (true) {
    if (redraw) {
      int visibleTotal = countVisible(g_listMode);
      if (visibleTotal == 0) sel = -1;
      if (visibleTotal > 0 && (sel < 0 || !recVisibleInMode(recList[sel], g_listMode))) sel = nearestVisibleIndex(sel, g_listMode);
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
        char recName[12];
        snprintf(recName, sizeof(recName), "REC_%04d", recList[i]);
        drawDseg14Text(d, 14, y, recName, on ? COL_GREEN : COL_DIM);
        char hk = hotkeyOf(recList[i]);
        bool imp = isImportantRec(recList[i]);
        if (hk && g_listMode != REC_SHORTCUT) {
          char keyLabel[8];
          snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
          drawDseg14Text(d, keyX, y, keyLabel, on ? COL_GREEN : COL_DIM);
        } else if (imp && g_listMode != REC_IMPORTANT) {
          drawDseg14Text(d, keyX, y, "IMP", on ? COL_GREEN : COL_DIM);
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
    if (keyDel() && !keyCtrl() && sel >= 0) {
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
      int hk = pressedHotkeyRec();
      if (hk > 0) {                                     // 绑定键=最高优先级, 立刻播放(可覆盖)
        g_listMode = REC_SHORTCUT;
        int r = playFlow(hk);
        if (r == R_RECORD) return R_RECORD;
        if (r == R_BACK) return R_BACK;
        if (r == R_LIST) {
          if (g_listReturnRec > 0) {
            int ni = recListIndexOf(g_listReturnRec);
            if (ni >= 0) { g_listMode = recKindOf(g_listReturnRec); sel = ni; }
            g_listReturnRec = 0;
          }
          redraw = true;
          if (!(g_carryDeleteRec > 0 && keyDel())) waitRelease();
          continue;
        }
        redraw = true; waitRelease();
      }
      else if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(COL_GREEN); waitRelease(); }
      else if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(COL_DIM); waitRelease(); }
      else if (keyCtrl() && keyDel()) {
        if (confirmDeleteUnmarked()) {
          int deleted = deleteUnmarkedRecordings();
          sel = recCount - 1;
          g_listMode = REC_NORMAL;
        }
        redraw = true; waitRelease();
      }
      else if (keyCtrl() && sel >= 0) {                             // Ctrl+键 = 绑定快捷键(字母/数字)
        if (keyAlt()) {
          int recNum = recList[sel];
          cleanFrictionForRec(recNum, false);
          redraw = true; waitRelease();
          continue;
        }
        if (keyEnter()) {
          bool ok = markRecordingImportant(recList[sel]);
          if (ok) g_listMode = REC_IMPORTANT;
          redraw = true; waitRelease();
          continue;
        }
        char bk = pressedBindKey();
        if (bk) {
          bool ok = markRecordingShortcut(recList[sel]);
          if (ok) setHotkey(bk, recList[sel]);
          if (ok) g_listMode = REC_SHORTCUT;
          redraw = true; waitRelease();
        }
      }
      else if (keySpace()) { return R_RECORD; }         // 空格=去录音
      else if (keyEsc())   { return R_BACK; }           // Esc=退出列表并息屏
      else if (keyLeft())  { g_listMode = (g_listMode + 2) % 3; sel = nearestVisibleIndex(sel, g_listMode); redraw = true; waitRelease(); }
      else if (keyRight()) { g_listMode = (g_listMode + 1) % 3; sel = nearestVisibleIndex(sel, g_listMode); redraw = true; waitRelease(); }
      else if (keyUp())    { if (sel >= 0) sel = prevVisibleIndex(sel, g_listMode); redraw = true; }
      else if (keyDown())  { if (sel >= 0) sel = nextVisibleIndex(sel, g_listMode); redraw = true; }
      else if (keyEnter() && sel >= 0) {
        int r = playFlow(recList[sel]);
        if (r == R_RECORD) return R_RECORD;             // 回放里按空格 -> 去录音
        if (r == R_BACK) return R_BACK;                 // 回放里按 Esc -> 息屏
        if (r == R_LIST) {
          if (g_listReturnRec > 0) {
            int ni = recListIndexOf(g_listReturnRec);
            if (ni >= 0) { g_listMode = recKindOf(g_listReturnRec); sel = ni; }
            g_listReturnRec = 0;
          }
          redraw = true;
          if (!(g_carryDeleteRec > 0 && keyDel())) waitRelease();
          continue;
        }
        redraw = true; waitRelease();
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
    delay(8);
  }
}

// 列表流程: 列表 <-> 录音 循环; Esc退出到息屏
void listFlow(int sel) {
  ensureHotkeysLoaded();
  while (true) {
    int r = listScreen(sel);
    if (r == R_RECORD) {
      int n = recordingScreen();
      if (n <= 0) {
        if (g_afterRecord == R_LIST) continue;
        return;
      }
      if (g_afterRecord == R_LIST) { sel = n; continue; }
      afterRecordingFlow(n);
      return;
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

// ---------- 息屏(轻睡眠): 关背光; 一直睡到"键盘中断"才醒; 空格=录音, 回车=列表 ----------
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
    d.setBrightness(120);
    M5Cardputer.update();
    if (keySpace()) wakeAction = R_RECORD;
    else wakeAction = R_LIST;
    autoRecordPending = true;
    waitRelease();
    return;
  }

  // 打开键盘芯片(TCA8418 @0x34)的按键中断 -> 按键时拉低 GPIO11; 可一直睡, 不必周期性醒
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x01, 400000);   // CFG: KE_IEN=1
  gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  while (true) {
    M5Cardputer.update();                                          // 排空键盘事件
    M5Cardputer.In_I2C.writeRegister8(0x34, 0x02, 0x03, 400000);  // 清中断标志 -> INT 线复位为高
    esp_light_sleep_start();                                       // 一直睡到 GPIO11 变低
    delay(6);
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (keySpace()) { wakeAction = R_RECORD; break; }
      if (keyEnter()) { wakeAction = R_LIST; break; }
    }
  }

  // 唤醒: 关掉唤醒源 + 键盘中断
  gpio_wakeup_disable(GPIO_NUM_11);
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x00, 400000);   // CFG: 关键盘中断
  d.setBrightness(120);
  if (wakeAction == R_RECORD) micWarmup(false, 16);  // 息屏唤醒复用预热链路, 减少开始爆音和等待
  autoRecordPending = true;
  waitRelease();
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // 开机不自动开扬声器(消除开机功放上电"爆音"), 播放时再手动开
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(SCREEN_ROT);
  M5Cardputer.Display.setBrightness(120);
  M5Cardputer.Display.setTextWrap(false);   // 关闭自动换行: 过长文字右侧截断, 不会换行掉出屏幕
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
    if (wakeAction == R_LIST) {
      wakeAction = R_RECORD;
      listFlow(0);
    } else {
      wakeAction = R_RECORD;
      int n = recordingScreen();
      afterRecordingFlow(n);
    }
    goSleep();
    return;
  }

  goSleep();
}
