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
#define WAVE_TOP    16     // 波形区顶 y
#define WAVE_BOT   109     // 波形区底 y  (余 26px 给计时器)
#define WAVE_H      93     // 波形区高度
#define WAVE_CY     62     // 波形中轴 y = (16+109)/2
static const int A_CY   = (WAVE_TOP + WAVE_CY) / 2;        // A线/轨道线中轴
static const int A_HALF = (WAVE_CY - WAVE_TOP) / 2 - 2;    // A线最大半幅
static const int B_CY   = (WAVE_CY + WAVE_BOT) / 2;        // B线/监听线中轴
static const int B_HALF = (WAVE_BOT - WAVE_CY) / 2 - 2;    // B线最大半幅

// ---------- 录音参数 ----------
static const uint32_t REC_RATE = 16000;  // 16kHz
static const size_t   REC_N    = 256;    // 每缓冲样本数 (~16ms, 小=波形更流畅)
static const size_t   PB_N     = 512;    // 播放缓冲样本数 (~32ms, B线更顺)
static int16_t recBuf[2][REC_N];
static int16_t pbBuf[2][PB_N];           // 播放双缓冲(放一块/读另一块, 避免覆盖破音)
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
static const int MAX_REC = 9999;

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
static const int MAX_HOTKEY = 24;
HotKey hotkeys[MAX_HOTKEY];
int hotkeyCount = 0;

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
uint8_t g_listMode = REC_NORMAL;  // 0普通 / 1快捷 / 2重要
static int nextRecHint = 0;  // 下一个录音编号缓存, 避免每次从 REC_0001 顺序探测
static const uint32_t DELETE_HOLD_MS = 1200;

// ---------- 按键小工具 ----------
// 取当前按下的第一个可绑定键(字母转小写, 或数字); 没有则返回 0
static char pressedBindKey() {
  for (char c : M5Cardputer.Keyboard.keysState().word) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 'a' && c <= 'z') return c;
    if (c >= '0' && c <= '9') return c;
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
static bool keyUp()    { return M5Cardputer.Keyboard.isKeyPressed(';'); }
static bool keyDown()  { return M5Cardputer.Keyboard.isKeyPressed('.'); }
static bool keyLeft()  { return M5Cardputer.Keyboard.isKeyPressed(','); }
static bool keyRight() { return M5Cardputer.Keyboard.isKeyPressed('/'); }
static bool keyVolUp() { return M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+'); }
static bool keyVolDn() { return M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_'); }

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
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);  // 提高模拟 PGA(更干净)
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
  int amp = (int)(rms * (3.0f * A_HALF) / 32767.0f) * sign;
  if (amp > A_HALF) amp = A_HALF;
  if (amp < -A_HALF) amp = -A_HALF;
  return (int8_t)amp;
}

// ---------- 通用 UI (横屏 240x135) ----------
// 顶栏: 标题(亮绿16) + 左侧小竖条点缀 + 下分隔线
static void drawHeader(const char *title) {
  auto &d = M5Cardputer.Display;
  d.fillRect(2, 5, 3, 13, COL_GREEN);          // 标题前的荧光绿小竖条(点缀)
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(10, 3);
  d.print(title);
  d.drawFastHLine(0, 21, d.width(), COL_DIM);
}

// 电量: 内容区右上角小字
static void drawBattery() {
  auto &d = M5Cardputer.Display;
  int bat = M5.Power.getBatteryLevel();
  uint16_t col = (bat <= 20) ? COL_RED : COL_DIM;
  d.fillRect(CONTENT_W - 34, 1, 34, 14, COL_BG);
  d.setFont(&fonts::efontCN_12);
  d.setTextColor(col, COL_BG);
  d.setCursor(CONTENT_W - 32, 2);
  d.printf("%d%%", bat);
}

// 底栏提示(小字, 上方一条分隔线)
static void drawFooter(const char *hint) {
  auto &d = M5Cardputer.Display;
  int y = d.height() - 15;
  d.drawFastHLine(0, y - 3, d.width(), COL_DIM);
  d.fillRect(0, y, d.width(), 15, COL_BG);
  d.setFont(&fonts::efontCN_12);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(6, y);
  d.print(hint);
}

static void drawActionToast(const char *msg, uint16_t col = COL_GREEN) {
  auto &d = M5Cardputer.Display;
  d.fillRect(86, 118, 68, 17, COL_BG);
  d.drawRect(86, 118, 68, 17, col);
  d.setFont(&fonts::efontCN_12);
  d.setTextColor(col, COL_BG);
  int x = 120 - (int)strlen(msg) * 3;
  if (x < 88) x = 88;
  d.setCursor(x, 121);
  d.print(msg);
}

static void drawCanvasToast(M5Canvas &cv, const char *msg, uint16_t col = COL_GREEN) {
  cv.fillRect(86, 58, 68, 20, COL_BG);
  cv.drawRect(86, 58, 68, 20, col);
  cv.setFont(&fonts::efontCN_12);
  cv.setTextColor(col, COL_BG);
  int x = 120 - (int)strlen(msg) * 3;
  if (x < 88) x = 88;
  cv.setCursor(x, 62);
  cv.print(msg);
  cv.pushSprite(0, 0);
}

static void showMsg(const char *title, const char *msg, uint16_t col) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader(title);
  d.setFont(&fonts::efontCN_16);
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
      char k = line.charAt(0);
      int idx = line.substring(2).toInt();
      if (k && idx > 0) { hotkeys[hotkeyCount].key = k; hotkeys[hotkeyCount].idx = idx; hotkeyCount++; }
    }
    f.close();
  }
  SD.end();
  if (migrated) saveHotkeys();
}

static void saveHotkeys() {
  if (!sdMount()) return;
  if (!SD.exists(SHORTCUT_DIR)) SD.mkdir(SHORTCUT_DIR);
  SD.remove(HOTKEY_PATH);
  File f = SD.open(HOTKEY_PATH, FILE_WRITE);
  if (f) {
    for (int i = 0; i < hotkeyCount; i++) f.printf("%c %d\n", hotkeys[i].key, hotkeys[i].idx);
    f.flush();
    f.close();
  }
  SD.end();
}

static int findHotkey(char k) {
  for (int i = 0; i < hotkeyCount; i++) if (hotkeys[i].key == k) return hotkeys[i].idx;
  return -1;
}

// 给某录音编号绑定一个键(同键覆盖, 同编号原有的键先清掉, 保证一键一录音)
static void setHotkey(char k, int idx) {
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

static void clearImportantBits() {
  memset(shortcutBits, 0, sizeof(shortcutBits));
  memset(importantBits, 0, sizeof(importantBits));
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

static void scanRecordingDir(const char *dirPath, uint8_t kind, int &maxIdx) {
  File dir = SD.open(dirPath);
  if (dir && dir.isDirectory()) {
    File e;
    while ((e = dir.openNextFile())) {
      int n = parseRecordingNumber(e.name());
      if (n > 0) {
        if (n > maxIdx) maxIdx = n;
        insertRecListSorted(n, kind);
      }
      e.close();
    }
    dir.close();
  }
}

static int nextRecordingIndex() {
  char p[40];
  if (nextRecHint > 0 && nextRecHint <= 9999) {
    recordingPathKind(nextRecHint, REC_NORMAL, p, sizeof(p));
    if (!SD.exists(p) && !recordingExistsKind(nextRecHint, REC_SHORTCUT) && !recordingExistsKind(nextRecHint, REC_IMPORTANT)) return nextRecHint;
  }

  int maxIdx = 0;
  recCount = 0;
  clearImportantBits();
  scanRecordingDir(REC_DIR, REC_NORMAL, maxIdx);
  scanRecordingDir(SHORTCUT_DIR, REC_SHORTCUT, maxIdx);
  scanRecordingDir(IMPORTANT_DIR, REC_IMPORTANT, maxIdx);

  int idx = maxIdx + 1;
  if (idx < 1) idx = 1;
  if (idx > 9999) idx = 9999;
  recordingPathKind(idx, REC_NORMAL, p, sizeof(p));
  while (idx < 9999 && (SD.exists(p) || recordingExistsKind(idx, REC_SHORTCUT) || recordingExistsKind(idx, REC_IMPORTANT))) {
    idx++;
    recordingPathKind(idx, REC_NORMAL, p, sizeof(p));
  }
  nextRecHint = idx;
  return idx;
}

static void scanRecordings() {
  recCount = 0;
  clearImportantBits();
  if (!sdMount()) return;
  if (!SD.exists(REC_DIR) && !SD.exists(SHORTCUT_DIR) && !SD.exists(IMPORTANT_DIR)) { SD.end(); return; }
  int maxIdx = 0;
  scanRecordingDir(REC_DIR, REC_NORMAL, maxIdx);
  scanRecordingDir(SHORTCUT_DIR, REC_SHORTCUT, maxIdx);
  scanRecordingDir(IMPORTANT_DIR, REC_IMPORTANT, maxIdx);
  nextRecHint = (recCount > 0 && recList[recCount - 1] < 9999) ? recList[recCount - 1] + 1 : 1;
  SD.end();
}

// ---------- 提示音 "滴" (淡入淡出) ----------
// (保留: 暂未使用; 需要时可在切到扬声器后调用以遮爆音)

// ---------- 切换编解码器到扬声器(含手动开 DAC) ----------
static bool copyFileOnSD(const char *from, const char *to) {
  File in = SD.open(from, FILE_READ);
  if (!in) return false;
  SD.remove(to);
  File out = SD.open(to, FILE_WRITE);
  if (!out) { in.close(); return false; }
  uint8_t buf[512];
  while (in.available()) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (out.write(buf, n) != (size_t)n) { in.close(); out.close(); return false; }
  }
  out.flush();
  in.close();
  out.close();
  return true;
}

static bool moveRecordingToKind(int recNum, uint8_t targetKind) {
  if (targetKind == REC_NORMAL) return false;
  if (!sdMount()) return false;
  const char *targetDir = (targetKind == REC_SHORTCUT) ? SHORTCUT_DIR : IMPORTANT_DIR;
  if (!SD.exists(targetDir)) SD.mkdir(targetDir);
  char src[40], dst[40];
  recordingPathKind(recNum, recKindOf(recNum), src, sizeof(src));
  recordingPathKind(recNum, targetKind, dst, sizeof(dst));
  bool ok = SD.exists(dst);
  if (!ok && SD.exists(src)) ok = SD.rename(src, dst);
  if (!ok && SD.exists(src)) {
    ok = copyFileOnSD(src, dst);
    if (ok) SD.remove(src);
  } else if (ok && SD.exists(src)) {
    SD.remove(src);
  }
  SD.end();
  if (ok) {
    setShortcutRec(recNum, targetKind == REC_SHORTCUT);
    setImportantRec(recNum, targetKind == REC_IMPORTANT);
  }
  return ok;
}

static bool markRecordingImportant(int recNum) {
  return moveRecordingToKind(recNum, REC_IMPORTANT);
}

static bool markRecordingShortcut(int recNum) {
  return moveRecordingToKind(recNum, REC_SHORTCUT);
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

static const int WAVE_BARS = 60;   // 60 × 4px = 240px (CONTENT_W)
static int8_t waveBars[WAVE_BARS]; // A线采样点: -A_HALF..+A_HALF
static uint8_t waveBarCounts[WAVE_BARS];

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
  cv.setFont(&fonts::efontCN_12);
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
  d.setFont(&fonts::efontCN_12);
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

// 播放画面: A线=轨道线/Timeline A, B线=监听线/Monitor B(播放缓冲)
static void drawPlaybackCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  // --- 波形区 ---
  cv.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);

  // A线: 时间轴波形进度。录制页和播放页共用同款小波形柱。
  cv.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(cv);

  // B线: 当前播放缓冲实时跳动
  cv.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  if (liveWave && liveN > 0) {
    int px = 0, py = B_CY;
    for (int x = 0; x < CONTENT_W; x++) {
      int idx = (int)((uint32_t)x * liveN / CONTENT_W);
      if (idx >= (int)liveN) idx = liveN - 1;
      int v = (int)((int32_t)liveWave[idx] * 3 * B_HALF / 32767);
      if (v > B_HALF) v = B_HALF; if (v < -B_HALF) v = -B_HALF;
      int y = B_CY - v;
      if (x > 0) cv.drawLine(px, py, x, y, COL_GREEN);
      px = x; py = y;
    }
  }

  // 播放头白线(只穿过A线区域)
  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  cv.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  // --- 计时器 (底部左侧) ---
  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  cv.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  cv.setFont(&fonts::Font4);
  cv.setTextColor(COL_GREEN, COL_BG);
  cv.setCursor(4, WAVE_BOT + 2);
  cv.printf("%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  cv.setFont(&fonts::efontCN_12);
  if (deleteHeldMs > 0) {
    drawDeleteProgress(cv, deleteHeldMs);
  } else if (paused) {
    cv.setTextColor(COL_DIM, COL_BG);
    cv.setCursor(CONTENT_W - 46, WAVE_BOT + 8);
    cv.print("PAUSE");
  } else {
    cv.setTextColor(COL_DIM, COL_BG);
    cv.setCursor(CONTENT_W - 44, WAVE_BOT + 8);
    cv.printf("/%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
  }
}

static void drawPlaybackAction(M5Canvas &cv, const char *msg, uint16_t col = COL_GREEN) {
  drawCanvasToast(cv, msg, col);
}

// ---------- 回放界面: 播放完自动回列表, 回车暂停/继续, 退格回列表, ;/.切换, +/- 音量, 长按Del删除, 空格去录音 ----------
// 返回 R_BACK(返回上一层), R_LIST(回列表), R_RECORD(去录音) 或 R_DELETE(删除当前录音)
int playbackScreen(const char *path, int recNum, int prevRec, int nextRec) {
  auto &d = M5Cardputer.Display;
  if (!sdMount()) { showMsg("播放", "SD 读取失败", COL_RED); return R_BACK; }
  File f = SD.open(path, FILE_READ);
  if (!f) { SD.end(); showMsg("播放", "打不开文件", COL_RED); return R_BACK; }
  uint32_t total = f.size();
  if (total <= 44) { f.close(); SD.end(); showMsg("播放", "文件为空", COL_RED); return R_BACK; }
  uint8_t hb[4]; f.seek(40); f.read(hb, 4);   // 按 WAV 头声明长度播放(尊重掐尾)
  uint32_t dataSize = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  uint32_t fileData = total - 44;
  if (dataSize == 0 || dataSize > fileData) dataSize = fileData;
  f.seek(44);
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));

  M5Canvas cv(&d);
  cv.createSprite(CONTENT_W, d.height());

  // 静态部分(画一次): 顶栏
  auto drawStatic = [&]() {
    cv.fillScreen(COL_BG);
    // 文件编号 (左上小字)
    cv.setFont(&fonts::efontCN_12);
    cv.setTextColor(COL_DIM, COL_BG);
    cv.setCursor(4, 2);
    cv.printf("REC_%04d", recNum);
    // 电量
    int bat = M5.Power.getBatteryLevel();
    cv.fillRect(CONTENT_W - 34, 0, 34, 14, COL_BG);
    cv.setTextColor(bat <= 20 ? COL_RED : COL_GREEN, COL_BG);
    cv.setCursor(CONTENT_W - 34, 2);
    cv.printf("%d%%", bat);
    // 波形区顶底分隔线
    cv.drawFastHLine(0, WAVE_TOP - 1, CONTENT_W, 0x0820);
    cv.drawFastHLine(0, WAVE_BOT + 1, CONTENT_W, 0x0820);
  };
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;
  bool paused = false;
  auto deleteHeldMs = [&]() -> uint32_t {
    if (delHoldStart == 0) return 0;
    uint32_t held = millis() - delHoldStart;
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
        else if (millis() - lastDelDraw > 60) { lastDelDraw = millis(); drawProgress(played); }
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
        else if (keyVolUp()) { playVol = min(255, playVol + 25); M5Cardputer.Speaker.setVolume(playVol); drawPlaybackAction(cv, "VOL+", COL_GREEN); }
        else if (keyVolDn()) { playVol = max(0,   playVol - 25); M5Cardputer.Speaker.setVolume(playVol); drawPlaybackAction(cv, "VOL-", COL_DIM); }
      }
    }
    if (stop) break;
    if (paused) { delay(8); continue; }

    if ((keyLeft() || keyRight()) && millis() - lastSeek > 120) {
      lastSeek = millis();
      M5Cardputer.Speaker.stop();
      uint32_t step = REC_RATE * 2;  // 约 1 秒 PCM 数据
      if (keyLeft()) played = (played > step) ? (played - step) : 0;
      if (keyRight()) played = (played + step < dataSize) ? (played + step) : dataSize;
      played &= ~1U;
      remaining = dataSize - played;
      f.seek(44 + played);
      fadeBytesLeft = REC_RATE * 2 * 80 / 1000;
      drawProgress(played);
      delay(15);
      continue;
    }
    if (remaining == 0) {
      if (M5Cardputer.Speaker.isPlaying(0) == 0) {
        if (!playDone) {
          playDone = true;
          drawProgress(dataSize);   // 显示 100% 最终状态
        }
        delay(80);
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
      d.setTextColor(COL_GREEN, COL_BG); d.setFont(&fonts::efontCN_16); d.setCursor(8, 38);
      d.printf("%d%%", (int)((uint64_t)processed * 100 / dataBytes));
    }
  }
  writeWavHeader(out, REC_RATE, outBytes);
  out.flush(); out.close();
  in.close();
  SD.remove(path);
  SD.rename(tmp, path);
  SD.end();
}

// ---------- 录音画面 (canvas: CONTENT_W × 135, 推送到 x=0) ----------
// A线: waveBars[] 小柱轨道(上半区)  B线: wave[]实时示波器(下半区)
void drawRecCanvas(M5Canvas &cv, uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  cv.fillScreen(COL_BG);

  // ── 顶栏 ────────────────────────────────────────────────────────
  if (ready || paused) {
    cv.fillCircle(8, 7, 5, COL_BG);
    cv.drawCircle(8, 7, 5, paused ? COL_DIM : 0x2000);
  } else {
    if (blink) cv.fillCircle(8, 7, 5, COL_RED);
    else { cv.fillCircle(8, 7, 5, COL_BG); cv.drawCircle(8, 7, 5, 0x2000); }
  }
  cv.setFont(&fonts::efontCN_12);
  cv.setTextColor((ready || paused) ? COL_DIM : COL_GREEN, COL_BG);
  cv.setCursor(18, 2);
  cv.print(ready ? "READY" : (paused ? "PAUSE" : "REC"));
  // 电量 (亮绿, 黑底确保可读)
  int bat = M5.Power.getBatteryLevel();
  cv.fillRect(CONTENT_W - 34, 0, 34, 14, COL_BG);
  cv.setTextColor((bat <= 20) ? COL_RED : COL_GREEN, COL_BG);
  cv.setCursor(CONTENT_W - 32, 2);
  cv.printf("%d%%", bat);
  cv.drawFastHLine(0, WAVE_TOP - 1, CONTENT_W, 0x0820);

  // ── A线: 滚动历史 (上半区, 中轴 y=39) ──────────────────────────
  cv.drawFastHLine(0, A_CY, CONTENT_W, 0x0440);
  drawTrackBars(cv);

  // ── B线: 实时示波器 (下半区, 中轴 y=85) ─────────────────────────
  cv.drawFastHLine(0, B_CY, CONTENT_W, 0x0440);
  if (wave) {
    int px = 0, py = B_CY;
    for (int x = 0; x < CONTENT_W; x++) {
      int idx = (int)((long)x * REC_N / CONTENT_W);
      int v = (int)((int32_t)wave[idx] * 3 * B_HALF / 32767);  // 3×放大
      if (v > B_HALF) v = B_HALF; if (v < -B_HALF) v = -B_HALF;
      int y = B_CY - v;
      if (x > 0) cv.drawLine(px, py, x, y, COL_GREEN);
      px = x; py = y;
    }
  }

  // ── 计时器 (底部左侧) ───────────────────────────────────────────
  uint32_t s = elapsedMs / 1000;
  cv.setFont(&fonts::Font4);
  cv.setTextColor(COL_GREEN, COL_BG);
  cv.setCursor(4, WAVE_BOT + 2);
  cv.printf("%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDeleteProgress(cv, deleteHeldMs);
}

// 录制. 开机自动进入; 调用此函数后创建文件并开始写入
int recordingScreen() {
  auto &d = M5Cardputer.Display;
  g_afterRecord = R_LIST;

  // 准备阶段仍显示 READY, 真正开始采样后才亮 REC 红点
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
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
    for (int i = 0; i < 6; i++) {
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
      } else if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keySpace()) {
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
        else if (keyEsc()) { drawCanvasToast(cv, "SLEEP", COL_DIM); g_afterRecord = R_BACK; stop = true; break; }
        else if (keyEnter()) { drawCanvasToast(cv, "SAVE", COL_GREEN); g_afterRecord = R_LIST; stop = true; break; }
        else if (keyAlt()) { drawCanvasToast(cv, "NOISE", COL_GREEN); g_afterRecord = R_NOISE; stop = true; break; }
        else if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) { if (recGain < 200) recGain += 4; }
        else if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) { if (recGain > 2) recGain -= 4; }
      }

      if (!ignoreStartKey) {
        if (keyDel()) {
          if (delHoldStart == 0) { delHoldStart = millis(); lastDelDraw = 0; }
          uint32_t held = millis() - delHoldStart;
          if (held >= DELETE_HOLD_MS) { cancelRec = true; g_afterRecord = R_BACK; stop = true; break; }
          if (millis() - lastDelDraw > 60) {
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
      drawRecCanvas(cv, elapsed, blink, filled, false, false, held);
      cv.pushSprite(0, 0);
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
  }
  cv.deleteSprite();
  // 掐尾 ~0.3s.
  uint32_t tailTrim = REC_RATE * 6 / 10;
  uint32_t effData = (dataBytes > tailTrim) ? (dataBytes - tailTrim) : 0;
  if (!cancelRec) writeWavHeader(f, REC_RATE, effData);
  f.close();
  if (cancelRec) {
    SD.remove(path);
    nextRecHint = idx;
  }
  SD.end();
  // 录音结束时停麦; 回调会写 0x0D=0xFC(爆音源), 立刻写回 0x01 把断电窗口压到 ~300µs
  micInputReady = false;
  M5Cardputer.Mic.end();
  const uint8_t ES = 0x18;
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);  // 立即恢复偏置(不加 delay)
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);  // CSM 上电
  if (cancelRec) return 0;
  insertRecListSorted(idx);
  return (effData > 0) ? idx : idx;   // 即使很短也保留, 返回编号
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

static bool confirmNoiseReduce(int recNum) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("降噪确认");
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(18, 42);
  d.printf("降噪 REC_%04d ?", recNum);
  d.setFont(&fonts::efontCN_12);
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
    d.setFont(&fonts::efontCN_16); d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(12, 56); d.print("还没有录音");
    d.setFont(&fonts::efontCN_12); d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(4, 120); d.print("空格录音  Esc息屏");
    waitRelease();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
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
  bool redraw = true;
  uint32_t delHoldStart = 0;
  uint32_t lastDelDraw = 0;

  const int rowH = 21;
  const int top = 26;
  int visRows = (d.height() - top - 18) / rowH;   // 底部留 18px 给提示
  if (visRows < 1) visRows = 1;

  waitRelease();
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
      drawBattery();

      // 标题行
      d.setFont(&fonts::efontCN_12);
      d.setTextColor(COL_GREEN, COL_BG);
      d.setCursor(4, 3);
      d.printf("%cREC %cKEY %cIMP  %d", g_listMode == REC_NORMAL ? '>' : ' ',
               g_listMode == REC_SHORTCUT ? '>' : ' ',
               g_listMode == REC_IMPORTANT ? '>' : ' ',
               visibleTotal);
      d.drawFastHLine(0, 15, CONTENT_W, COL_DIM);

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
        d.setFont(&fonts::efontCN_16);
        d.setTextColor(on ? COL_GREEN : COL_DIM, bg);
        d.setCursor(14, y);
        d.printf("REC_%04d", recList[i]);
        if (isShortcutRec(recList[i])) d.print(">");
        else if (isImportantRec(recList[i])) d.print("*");
        // 快捷键标签
        char hk = hotkeyOf(recList[i]);
        if (hk) {
          d.setFont(&fonts::efontCN_12);
          d.setTextColor(on ? COL_GREEN : COL_DIM, bg);
          d.setCursor(CONTENT_W - 26, y + 3);
          d.printf("[%c]", dispKey(hk));
        }
      }
      // 更多项箭头
      d.setFont(&fonts::efontCN_12); d.setTextColor(COL_DIM, COL_BG);
      if (firstOrdinal > 0)                    { d.setCursor(CONTENT_W - 10, top); d.print("^"); }
      if (firstOrdinal + visRows < visibleTotal) { d.setCursor(CONTENT_W - 10, top + (visRows - 1) * rowH); d.print("v"); }
      // 底部操作提示
      d.drawFastHLine(0, 120, CONTENT_W, COL_DIM);
      d.setCursor(4, 122); d.print(";/.选 回车放 Esc退 长Del删");
    }

    M5Cardputer.update();
    if (keyDel() && sel >= 0) {
      if (delHoldStart == 0) delHoldStart = millis();
      uint32_t held = millis() - delHoldStart;
      if (held >= DELETE_HOLD_MS) {
        int recNum = recList[sel];
        drawActionToast("DEL", COL_RED);
        deleteRecording(recNum);
        removeRecListAt(sel);
        if (recCount == 0) return R_BACK;
        if (sel >= recCount) sel = recCount - 1;
        delHoldStart = 0;
        lastDelDraw = 0;
        redraw = true;
        waitRelease();
      } else if (millis() - lastDelDraw > 60) {
        lastDelDraw = millis();
        drawDeleteProgressOnDisplay(held);
      }
    } else {
      if (delHoldStart != 0) {
        delHoldStart = 0;
        lastDelDraw = 0;
        redraw = true;
      }
    }
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      int hk = pressedHotkeyRec();
      if (hk > 0) {                                     // 绑定键=最高优先级, 立刻播放(可覆盖)
        drawActionToast("PLAY", COL_GREEN);
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
          redraw = true; waitRelease(); continue;
        }
        redraw = true; waitRelease();
      }
      else if (keyCtrl() && sel >= 0) {                             // Ctrl+键 = 绑定快捷键(字母/数字)
        if (keyEnter()) {
          bool ok = markRecordingImportant(recList[sel]);
          if (ok) g_listMode = REC_IMPORTANT;
          drawActionToast(ok ? "IMP" : "ERR", ok ? COL_GREEN : COL_RED);
          redraw = true; waitRelease();
          continue;
        }
        char bk = pressedBindKey();
        if (bk) {
          bool ok = markRecordingShortcut(recList[sel]);
          if (ok) setHotkey(bk, recList[sel]);
          if (ok) g_listMode = REC_SHORTCUT;
          drawActionToast(ok ? "BIND" : "ERR", ok ? COL_GREEN : COL_RED);
          redraw = true; waitRelease();
        }
      }
      else if (keySpace()) { drawActionToast("REC", COL_RED); return R_RECORD; }         // 空格=去录音
      else if (keyEsc())   { drawActionToast("SLEEP", COL_DIM); return R_BACK; }           // Esc=退出列表并息屏
      else if (keyLeft())  { g_listMode = (g_listMode + 2) % 3; sel = nearestVisibleIndex(sel, g_listMode); redraw = true; waitRelease(); }
      else if (keyRight()) { g_listMode = (g_listMode + 1) % 3; sel = nearestVisibleIndex(sel, g_listMode); redraw = true; waitRelease(); }
      else if (keyUp())    { if (sel >= 0) sel = prevVisibleIndex(sel, g_listMode); redraw = true; }
      else if (keyDown())  { if (sel >= 0) sel = nextVisibleIndex(sel, g_listMode); redraw = true; }
      else if (keyEnter() && sel >= 0) {
        drawActionToast("PLAY", COL_GREEN);
        int r = playFlow(recList[sel]);
        if (r == R_RECORD) return R_RECORD;             // 回放里按空格 -> 去录音
        if (r == R_BACK) return R_BACK;                 // 回放里按 Esc -> 息屏
        if (r == R_LIST) {
          if (g_listReturnRec > 0) {
            int ni = recListIndexOf(g_listReturnRec);
            if (ni >= 0) { g_listMode = recKindOf(g_listReturnRec); sel = ni; }
            g_listReturnRec = 0;
          }
          redraw = true; waitRelease(); continue;
        }
        redraw = true; waitRelease();
      }
      else if (keyAlt() && sel >= 0) {
        drawActionToast("NOISE", COL_GREEN);
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
static void micWarmup() {
  // 开机/唤醒的第一条录音也必须走完整输入链路配置; 否则首次录音增益会偏低。
  micInputReady = false;
  if (!prepareMicInput()) return;
  forceMicRearm = true;
  static int16_t warm[REC_N];
  for (int i = 0; i < 24; i++) {
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

  // 静音 ES8311 输出路径(不掉电 0x0D/0x12): 防止睡眠瞬态被无 mute 脚功放放大
  const uint8_t ES = 0x18;
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(0);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0x00, 400000);  // 先断开 DAC->HP mixer
  delay(2);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x00, 400000);  // 关 HP drive
  delay(5);

  d.fillScreen(COL_BG);
  d.setBrightness(0);          // 关背光 = 最大省电点
  delay(10);

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
  if (wakeAction == R_RECORD) micWarmup();    // 只有立刻录音才预热; 回车进列表要快
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

  // 预热麦克风(开机空跑一次, 消冷启动不稳定; 之后保持常开, 见 micWarmup 内说明)
  micWarmup();

  // 预热 SD 卡 + 读取快捷键绑定
  if (sdMount()) {
    File wf = SD.open("/.warm", FILE_WRITE);
    if (wf) { uint8_t z[512] = {0}; for (int i = 0; i < 16; i++) wf.write(z, sizeof(z)); wf.close(); SD.remove("/.warm"); }
    SD.end();
  }
  loadHotkeys();

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
