/*
 * 我的工具箱 (My Toolbox) for M5Stack Cardputer-Adv
 * 风格: 纯黑背景 + 黑客帝国荧光绿 + 线性 + 中文界面 + 竖屏(USB-C 口朝上)
 *
 * 录音应用交互:
 *   主屏: 空格=录音; ;/. 上下选菜单, 回车进入"录音列表"; 按已绑定的字母键=直接播放该录音
 *   录制中: 空格=暂停/继续; 回车=结束(存盘并进入列表); W/S=音量
 *   录音列表: ;/. 上下选; 回车=播放; 退格=返回主屏; Z 再按字母=给该录音绑定快捷键; N=降噪
 *   回放: 回车=暂停/继续; +/- (= 与 - 键)=音量; 退格=返回
 *
 * 方向键(物理): ; = 上, . = 下, , = 左, / = 右
 */
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

// ---------- 屏幕方向 ----------
// 竖屏: 0 或 2. 若上下颠倒, 改另一个即可.
#define SCREEN_ROT 2

// ---------- 配色 ----------
#define COL_BG    0x0000   // 纯黑
#define COL_GREEN 0x07E0   // 荧光绿(亮)
#define COL_DIM   0x03A0   // 暗绿
#define COL_RED   0xF800   // 录音红点

// ---------- 录音参数 ----------
static const uint32_t REC_RATE = 16000;  // 16kHz
static const size_t   REC_N    = 256;    // 每缓冲样本数 (~16ms, 小=波形更流畅)
static int16_t recBuf[2][REC_N];
static int16_t pbBuf[2][1024];           // 播放双缓冲(放一块/读另一块, 避免覆盖破音)
int recGain = 50;                        // 录音软件增益默认值(W/S 现场可调, 带削波保护)
int playVol = 200;                       // 回放音量(+/- 可调, 0..255)

// SD 引脚(运行时由 M5Unified 按机型给出, 失败则回退到 Adv 已知值)
int sdSCLK = 40, sdMISO = 39, sdMOSI = 14, sdCS = 12;

// ---------- 快捷键绑定 (字母键 -> 录音编号), 存到 SD 卡 /REC/keys.txt ----------
struct HotKey { char key; int idx; };
static const int MAX_HOTKEY = 24;
HotKey hotkeys[MAX_HOTKEY];
int hotkeyCount = 0;

// 主屏菜单光标 / 闪烁
int homeSel = 0;             // 0=开始录音, 1=录音列表
uint32_t lastBlink = 0;
bool curOn = false;
uint32_t lastBat = 0;

// ---------- SD 辅助 ----------
static bool sdMount() {
  SPI.begin(sdSCLK, sdMISO, sdMOSI, sdCS);
  return SD.begin(sdCS, SPI, 25000000);
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
int g_nextPlay = 0;  // 配合 R_PLAY: 要切换去播放的录音编号

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
static bool keySpace() { return M5Cardputer.Keyboard.isKeyPressed(' '); }
static bool keyDel()   { return M5Cardputer.Keyboard.keysState().del; }
static bool keyEnter() { return M5Cardputer.Keyboard.keysState().enter; }
static bool keyUp()    { return M5Cardputer.Keyboard.isKeyPressed(';'); }
static bool keyDown()  { return M5Cardputer.Keyboard.isKeyPressed('.'); }
static bool keyVolUp() { return M5Cardputer.Keyboard.isKeyPressed('=') || M5Cardputer.Keyboard.isKeyPressed('+'); }
static bool keyVolDn() { return M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_'); }

// 等所有键松开
static void waitRelease() {
  do { M5Cardputer.update(); delay(8); } while (M5Cardputer.Keyboard.isPressed());
}

// ---------- 通用 UI ----------
static void drawHeader(const char *title) {
  auto &d = M5Cardputer.Display;
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5);
  d.print(title);
  d.drawFastHLine(0, 24, d.width(), COL_DIM);
}

static void drawBattery() {
  auto &d = M5Cardputer.Display;
  int y = d.height() - 18;
  d.setFont(&fonts::efontCN_16);
  d.fillRect(0, y, d.width(), 18, COL_BG);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(6, y);
  d.printf("BAT %d%%", M5.Power.getBatteryLevel());
}

static void showMsg(const char *title, const char *msg, uint16_t col) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader(title);
  d.setTextColor(col, COL_BG);
  d.setCursor(8, 40);
  d.print(msg);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(8, d.height() - 40);
  d.print("按任意键返回");
  waitRelease();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    delay(8);
  }
}

// ---------- 快捷键绑定: 读/写 SD ----------
static void loadHotkeys() {
  hotkeyCount = 0;
  if (!sdMount()) return;
  File f = SD.open("/REC/keys.txt", FILE_READ);
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
}

static void saveHotkeys() {
  if (!sdMount()) return;
  if (!SD.exists("/REC")) SD.mkdir("/REC");
  SD.remove("/REC/keys.txt");
  File f = SD.open("/REC/keys.txt", FILE_WRITE);
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
static const int MAX_REC = 200;
int recList[MAX_REC];
int recCount = 0;

static void scanRecordings() {
  recCount = 0;
  if (!sdMount()) return;
  if (!SD.exists("/REC")) { SD.end(); return; }
  char p[40];
  int misses = 0;
  for (int i = 1; i <= 9999 && recCount < MAX_REC; i++) {
    snprintf(p, sizeof(p), "/REC/REC_%04d.wav", i);
    if (SD.exists(p)) { recList[recCount++] = i; misses = 0; }
    else if (++misses > 5) break;   // 编号连续, 连查到几个空缺就停(避免一路查到9999卡死)
  }
  SD.end();
}

// ---------- 提示音 "滴" (淡入淡出) ----------
// (保留: 暂未使用; 需要时可在切到扬声器后调用以遮爆音)

// ---------- 切换编解码器到扬声器(含手动开 DAC) ----------
static void speakerOn() {
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
}

// ---------- 回放界面: 回车暂停/继续, +/- 音量, 退格返回, 空格去录音 ----------
// 返回 R_BACK(返回上一层) 或 R_RECORD(去录音)
int playbackScreen(const char *path, int recNum) {
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

  speakerOn();

  const int barY = 64, barW = d.width() - 16;
  const int vbX = 48, vbY = 92, vbW = d.width() - 56;

  // 静态部分(画一次)
  auto drawStatic = [&](bool paused) {
    d.fillScreen(COL_BG);
    drawHeader(paused ? "已暂停" : "播放中");
    d.setFont(&fonts::efontCN_16);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(8, 32); d.printf("REC_%04d", recNum);
    d.drawRect(8, barY, barW, 8, COL_DIM);
    d.setTextColor(COL_GREEN, COL_BG);
    d.setCursor(8, 90); d.print("音量");
    d.drawRect(vbX, vbY, vbW, 8, COL_DIM);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(8, d.height() - 64); d.print(paused ? "回车=继续" : "回车=暂停");
    d.setCursor(8, d.height() - 44); d.print("+/-音量 空格录音");
    d.setCursor(8, d.height() - 24); d.print("退格=返回");
  };
  auto drawProgress = [&](uint32_t played) {
    int prog = dataSize ? (int)((uint64_t)played * (barW - 2) / dataSize) : 0;
    d.fillRect(9, barY + 1, barW - 2, 6, COL_BG);
    d.fillRect(9, barY + 1, prog, 6, COL_GREEN);
  };
  auto drawVol = [&]() {
    d.fillRect(vbX + 1, vbY + 1, vbW - 2, 6, COL_BG);
    d.fillRect(vbX + 1, vbY + 1, playVol * (vbW - 2) / 255, 6, COL_GREEN);
  };

  bool stop = false, paused = false;
  int ret = R_BACK;
  uint32_t played = 0, remaining = dataSize, lastDraw = 0;
  int pi = 0;

  drawStatic(false); drawProgress(0); drawVol();
  waitRelease();

  while (!stop) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      int hk = pressedHotkeyRec();
      if (hk > 0) { g_nextPlay = hk; ret = R_PLAY; stop = true; }   // 按到别的绑定键=立刻覆盖播放
      else if (keySpace()) { ret = R_RECORD; stop = true; }   // 空格=去录音
      else if (keyDel()) { ret = R_BACK; stop = true; }       // 退格=返回
      else if (keyEnter()) { paused = !paused; drawStatic(paused); drawProgress(played); drawVol(); waitRelease(); }
      else if (keyVolUp()) { playVol = (playVol + 25 > 255) ? 255 : playVol + 25; M5Cardputer.Speaker.setVolume(playVol); drawVol(); }
      else if (keyVolDn()) { playVol = (playVol - 25 < 0) ? 0 : playVol - 25; M5Cardputer.Speaker.setVolume(playVol); drawVol(); }
    }
    if (stop) break;
    if (paused) { delay(8); continue; }

    if (remaining == 0) {
      if (M5Cardputer.Speaker.isPlaying(0) == 0) break;   // 放完
      delay(8); continue;
    }
    if (M5Cardputer.Speaker.isPlaying(0) >= 2) { delay(2); continue; }
    size_t want = remaining < sizeof(pbBuf[pi]) ? remaining : sizeof(pbBuf[pi]);
    int got = f.read((uint8_t *)pbBuf[pi], want);
    if (got <= 0) { remaining = 0; continue; }
    remaining -= got;
    M5Cardputer.Speaker.playRaw(pbBuf[pi], got / 2, REC_RATE, false, 1, 0, false);
    pi ^= 1;
    played += got;
    if (millis() - lastDraw > 200) { lastDraw = millis(); drawProgress(played); }
  }
  M5Cardputer.Speaker.stop();
  f.close();
  SD.end();
  return ret;
}

// 播放流程: 支持"播放中按别的绑定键 -> 立刻切到那条"(覆盖). 返回 R_BACK 或 R_RECORD.
int playFlow(int recNum) {
  while (true) {
    char p[40]; snprintf(p, sizeof(p), "/REC/REC_%04d.wav", recNum);
    int r = playbackScreen(p, recNum);
    if (r == R_PLAY) { recNum = g_nextPlay; continue; }
    return r;
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

// ---------- 录制中: 离屏画布画竖屏声波(双缓冲不闪) ----------
void drawRecCanvas(M5Canvas &cv, uint32_t elapsedMs, bool blink, bool paused, int16_t *wave) {
  int W = cv.width(), H = cv.height();
  cv.fillScreen(COL_BG);
  if (!paused && blink) cv.fillCircle(9, 9, 5, COL_RED);
  cv.setFont(&fonts::efontCN_16);
  cv.setTextColor(paused ? COL_DIM : COL_GREEN, COL_BG);
  cv.setCursor(20, 1);
  cv.print(paused ? "已暂停" : "录音中");
  uint32_t s = elapsedMs / 1000;
  cv.setTextColor(COL_DIM, COL_BG);
  cv.setCursor(2, 22);
  cv.printf("%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  // 声波(示波器): 横跨整宽
  int wfTop = 44, wfBot = H - 4;
  int cy = (wfTop + wfBot) / 2, half = (wfBot - wfTop) / 2;
  cv.drawFastHLine(0, cy, W, COL_DIM);
  int px = 0, py = cy;
  for (int x = 0; x < W; x++) {
    int idx = (int)((long)x * REC_N / W);
    int v = wave[idx] >> 10;
    if (v > half) v = half;
    if (v < -half) v = -half;
    int y = cy - v;
    if (x > 0) cv.drawLine(px, py, x, y, COL_GREEN);
    px = x; py = y;
  }
}

// 录制. 返回新录音编号(>0); 0=失败/无内容
int recordingScreen() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("录音机");

  if (!sdMount()) { showMsg("录音机", "未检测到 SD 卡", COL_RED); return 0; }
  if (!SD.exists("/REC")) SD.mkdir("/REC");
  int idx = 1; char path[40];
  do { snprintf(path, sizeof(path), "/REC/REC_%04d.wav", idx); if (!SD.exists(path)) break; idx++; } while (idx < 9999);
  File f = SD.open(path, FILE_WRITE);
  if (!f) { SD.end(); showMsg("录音机", "无法写入文件", COL_RED); return 0; }
  writeWavHeader(f, REC_RATE, 0);

  // 切到麦克风(常开时 Mic.begin 是空操作, 不爆音; 刚播放过则真正重开)
  M5Cardputer.Speaker.end();
  {
    auto mc = M5Cardputer.Mic.config();
    mc.magnification = 1; mc.noise_filter_level = 3;
    M5Cardputer.Mic.config(mc);
  }
  if (!M5Cardputer.Mic.begin()) { f.close(); SD.end(); showMsg("录音机", "麦克风启动失败", COL_RED); return 0; }
  delay(40);
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);  // 提高模拟 PGA(更干净)

  waitRelease();

  M5Canvas cv(&d);
  cv.createSprite(d.width(), d.height() - 54);   // 顶部 0..30 标题, 底部留提示

  uint32_t dataBytes = 0;
  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  M5Cardputer.Mic.record(recBuf[1], REC_N, REC_RATE);
  int b = 0;
  uint32_t startMs = millis(), pausedMs = 0, pauseStart = 0, lastDraw = 0;
  bool stop = false, paused = false;
  uint32_t bufCount = 0;
  int32_t lpf = 0;
  const uint32_t HEAD_SKIP = 13;   // 掐头 ~0.2s, 去掉"开始键"声

  while (!stop) {
    while (M5Cardputer.Mic.isRecording() >= 2) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyEnter()) { stop = true; break; }
        else if (M5Cardputer.Keyboard.isKeyPressed(' ')) {       // 空格=暂停/继续
          paused = !paused;
          if (paused) pauseStart = millis();
          else { pausedMs += millis() - pauseStart; }
          lastDraw = 0; waitRelease();
        }
        else if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) { if (recGain < 200) recGain += 4; }
        else if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) { if (recGain > 2) recGain -= 4; }
      }
      delay(1);
    }
    if (stop) break;
    int16_t *filled = recBuf[b];
    for (size_t k = 0; k < REC_N; k++) {                         // 增益->低通->软限幅
      int32_t x = (int32_t)filled[k] * recGain;
      lpf += (int32_t)(((int64_t)(x - lpf) * 160) >> 8);
      float yf = 32767.0f * tanhf((float)lpf / 32767.0f);
      filled[k] = (int16_t)yf;
    }
    if (!paused) {
      bufCount++;
      if (bufCount > HEAD_SKIP) { f.write((uint8_t *)filled, REC_N * sizeof(int16_t)); dataBytes += REC_N * sizeof(int16_t); }
    }
    uint32_t now = millis();
    if (now - lastDraw > 33) {
      lastDraw = now;
      bool blink = ((now / 400) % 2) == 0;
      uint32_t elapsed = (paused ? pauseStart : now) - startMs - pausedMs;
      drawRecCanvas(cv, elapsed, blink, paused, filled);
      cv.pushSprite(0, 30);
      // 底部提示(屏幕直接画, 不在画布里)
      d.setFont(&fonts::efontCN_16); d.setTextColor(COL_DIM, COL_BG);
      d.fillRect(0, d.height() - 22, d.width(), 22, COL_BG);
      d.setCursor(4, d.height() - 20);
      d.printf("空格%s 回车存", paused ? "继续" : "暂停");
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
  }
  cv.deleteSprite();
  // 停录不关麦(避免掉电爆音). 掐尾 ~0.3s.
  uint32_t tailTrim = REC_RATE * 6 / 10;
  uint32_t effData = (dataBytes > tailTrim) ? (dataBytes - tailTrim) : 0;
  writeWavHeader(f, REC_RATE, effData);
  f.flush(); f.close(); SD.end();
  return (effData > 0) ? idx : idx;   // 即使很短也保留, 返回编号
}

// ---------- 录音列表: ;/.选, 回车放, Ctrl+键绑定, N降噪, 空格录音, 退格返回 ----------
// 返回 R_BACK(返回主屏) 或 R_RECORD(去录音)
int listScreen(int selectIdx) {
  auto &d = M5Cardputer.Display;
  scanRecordings();

  // 空列表: 也允许 空格=录音 / 退格=返回
  if (recCount == 0) {
    d.fillScreen(COL_BG); drawHeader("录音列表");
    d.setFont(&fonts::efontCN_16); d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(8, 48);  d.print("还没有录音");
    d.setCursor(8, 84);  d.print("空格=录音");
    d.setCursor(8, 104); d.print("退格=返回");
    waitRelease();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keySpace()) return R_RECORD;
        if (keyDel())   return R_BACK;
      }
      delay(8);
    }
  }

  int sel = 0;
  if (selectIdx > 0) for (int i = 0; i < recCount; i++) if (recList[i] == selectIdx) { sel = i; break; }
  bool redraw = true;

  const int rowH = 22;
  const int top = 30;
  int visRows = (d.height() - top - 58) / rowH;   // 底部留 58px 给提示
  if (visRows < 1) visRows = 1;

  waitRelease();
  while (true) {
    int first = sel - visRows / 2;
    if (first < 0) first = 0;
    if (first > recCount - visRows) first = recCount - visRows;
    if (first < 0) first = 0;

    if (redraw) {
      redraw = false;
      d.fillScreen(COL_BG);
      char title[24]; snprintf(title, sizeof(title), "录音列表 %d", recCount);
      drawHeader(title);
      for (int r = 0; r < visRows && first + r < recCount; r++) {
        int i = first + r;
        int y = top + r * rowH;
        bool on = (i == sel);
        if (on) d.fillRoundRect(2, y - 1, d.width() - 4, rowH - 2, 3, 0x0140);
        d.setFont(&fonts::efontCN_16);
        d.setTextColor(on ? COL_GREEN : COL_DIM, on ? 0x0140 : COL_BG);
        d.setCursor(6, y + 2);
        char hk = hotkeyOf(recList[i]);
        if (hk) d.printf("REC_%04d [%c]", recList[i], dispKey(hk));
        else    d.printf("REC_%04d", recList[i]);
      }
      // 底部提示(短行, 不超宽)
      int hy = d.height() - 56;
      d.setFont(&fonts::efontCN_16); d.setTextColor(COL_DIM, COL_BG);
      d.fillRect(0, hy, d.width(), 56, COL_BG);
      d.setCursor(4, hy);      d.print(";/.选 回车放");
      d.setCursor(4, hy + 18); d.print("Ctrl+键绑定 N降噪");
      d.setCursor(4, hy + 36); d.print("空格录音 退格返回");
    }

    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      int hk = pressedHotkeyRec();
      if (hk > 0) {                                     // 绑定键=最高优先级, 立刻播放(可覆盖)
        int r = playFlow(hk);
        if (r == R_RECORD) return R_RECORD;
        redraw = true; waitRelease();
      }
      else if (keyCtrl()) {                             // Ctrl+键 = 绑定快捷键(字母/数字)
        char bk = pressedBindKey();
        if (bk) { setHotkey(bk, recList[sel]); redraw = true; waitRelease(); }
      }
      else if (keySpace()) { return R_RECORD; }         // 空格=去录音
      else if (keyDel())   { return R_BACK; }           // 退格=返回主屏
      else if (keyUp())    { if (sel > 0) sel--; redraw = true; waitRelease(); }
      else if (keyDown())  { if (sel < recCount - 1) sel++; redraw = true; waitRelease(); }
      else if (keyEnter()) {
        int r = playFlow(recList[sel]);
        if (r == R_RECORD) return R_RECORD;             // 回放里按空格 -> 去录音
        redraw = true; waitRelease();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N')) {
        char p[40]; snprintf(p, sizeof(p), "/REC/REC_%04d.wav", recList[sel]);
        noiseReduce(p); redraw = true; waitRelease();
      }
    }
    delay(8);
  }
}

// 列表流程: 列表 <-> 录音 循环; 退格回主屏
void listFlow(int sel) {
  while (true) {
    int r = listScreen(sel);
    if (r == R_RECORD) { int n = recordingScreen(); sel = n; continue; }
    return;
  }
}

// ---------- 主屏 ----------
void drawHome() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawHeader("录音机");
  d.setFont(&fonts::efontCN_16);
  const char *items[2] = {"开始录音", "录音列表"};
  for (int i = 0; i < 2; i++) {
    int y = 40 + i * 26;
    bool on = (i == homeSel);
    d.setTextColor(on ? COL_GREEN : COL_DIM, COL_BG);
    d.setCursor(8, y);
    d.printf("%s %s", on ? ">" : " ", items[i]);
  }
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(8, 104);
  d.print("空格=录音");
  d.setCursor(8, 124);
  d.print("绑定键=播放");
  d.drawFastHLine(0, d.height() - 20, d.width(), COL_DIM);
  drawBattery();
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

  // 预热麦克风: 开机空跑一次录音(录几帧丢弃), 消耗掉冷启动首次会话的不稳定.
  // 关键: 预热后【不关麦】, 让 ES8311 保持上电 —— 关麦会写"模拟掉电+整体掉电"寄存器,
  // 那个电压跳变正是"开机第二声爆音"的来源; 常开还能让第一次录音零爆音.
  {
    auto mc = M5Cardputer.Mic.config();
    mc.magnification = 1; mc.noise_filter_level = 3;
    M5Cardputer.Mic.config(mc);
  }
  M5Cardputer.Mic.begin();
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);
  {
    static int16_t warm[REC_N];
    for (int i = 0; i < 24; i++) {
      M5Cardputer.Mic.record(warm, REC_N, REC_RATE);
      while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    }
  }
  // (此处不再 Mic.end(): 保持麦克风常开 —— 省掉开机第二声, 并让第一次录音零爆音)

  // 预热 SD 卡 + 读取快捷键绑定
  if (sdMount()) {
    File wf = SD.open("/.warm", FILE_WRITE);
    if (wf) { uint8_t z[512] = {0}; for (int i = 0; i < 16; i++) wf.write(z, sizeof(z)); wf.close(); SD.remove("/.warm"); }
    SD.end();
  }
  loadHotkeys();

  drawHome();
}

void loop() {
  M5Cardputer.update();

  uint32_t now = millis();
  if (now - lastBat > 5000) { lastBat = now; drawBattery(); }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    // 已绑定的键(字母/数字) -> 最高优先级, 立刻播放(可覆盖)
    int hk = pressedHotkeyRec();
    if (hk > 0) {
      int r = playFlow(hk);
      if (r == R_RECORD) { int n = recordingScreen(); listFlow(n); }   // 回放里按空格 -> 录音
      homeSel = 0; drawHome(); waitRelease(); return;
    }
    if (keySpace()) {                                            // 空格=录音 -> 列表
      int n = recordingScreen();
      listFlow(n);
      homeSel = 0; drawHome(); waitRelease();
    } else if (keyUp())   { homeSel = 0; drawHome(); waitRelease(); }
      else if (keyDown()) { homeSel = 1; drawHome(); waitRelease(); }
      else if (keyEnter()) {
        if (homeSel == 0) { int n = recordingScreen(); listFlow(n); }
        else { listFlow(0); }
        homeSel = 0; drawHome(); waitRelease();
      }
  }
  delay(8);
}
