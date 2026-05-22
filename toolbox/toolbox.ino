/*
 * 我的工具箱 (My Toolbox) for M5Stack Cardputer-Adv
 * 风格: 纯黑背景 + 黑客帝国荧光绿 + 线性 + 中文界面
 * v1.1 工具: 按 L 录音 -> 存 WAV 到 SD;  按 P 回放最近一段录音
 */
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

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

// SD 引脚(运行时由 M5Unified 按机型给出, 失败则回退到 Adv 已知值)
int sdSCLK = 40, sdMISO = 39, sdMOSI = 14, sdCS = 12;

char lastRecPath[40] = "";

// 主页光标闪烁
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

// ---------- 通用 UI ----------
static void waitAnyKey() {
  do { M5Cardputer.update(); delay(10); } while (M5Cardputer.Keyboard.isPressed());
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    delay(10);
  }
}

static void showMsg(const char *title, const char *msg, uint16_t col) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5);
  d.print(title);
  d.drawFastHLine(0, 26, d.width(), COL_DIM);
  d.setTextColor(col, COL_BG);
  d.setCursor(10, 44);
  d.print(msg);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(10, 88);
  d.print("按任意键返回");
  waitAnyKey();
}

static void drawBattery() {
  auto &d = M5Cardputer.Display;
  int y = d.height() - 17;
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_DIM, COL_BG);
  d.fillRect(0, y, d.width(), 17, COL_BG);
  d.setCursor(6, y);
  d.printf("BAT %d%%", M5.Power.getBatteryLevel());
}

void drawHome() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5);
  d.print("我的工具箱");
  d.drawFastHLine(0, 26, d.width(), COL_DIM);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(10, 36);
  d.print("[空格] 录音");
  d.setCursor(10, 60);
  d.print("[回车] 播放最近");
  d.drawFastHLine(0, d.height() - 19, d.width(), COL_DIM);
  drawBattery();
}

// ---------- 找最近一段录音 ----------
static bool findLatestRec(char *out, size_t outlen) {
  if (!sdMount()) return false;
  int last = 0;
  char p[40];
  for (int i = 1; i <= 9999; i++) {
    snprintf(p, sizeof(p), "/REC/REC_%04d.wav", i);
    if (SD.exists(p)) last = i;
    else if (last > 0) break;
  }
  SD.end();
  if (last == 0) return false;
  snprintf(out, outlen, "/REC/REC_%04d.wav", last);
  return true;
}

// ---------- 播放 WAV ----------
void playWav(const char *path) {
  auto &d = M5Cardputer.Display;
  if (!sdMount()) { showMsg("播放", "SD 读取失败", COL_RED); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { SD.end(); showMsg("播放", "打不开文件", COL_RED); return; }
  uint32_t total = f.size();
  if (total <= 44) { f.close(); SD.end(); showMsg("播放", "文件为空", COL_RED); return; }
  // 按 WAV 头声明的数据长度播放(尊重掐尾, 不把多余尾巴/按键声播出来)
  uint8_t hb[4]; f.seek(40); f.read(hb, 4);
  uint32_t dataSize = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8) | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
  uint32_t fileData = total - 44;
  if (dataSize == 0 || dataSize > fileData) dataSize = fileData;
  f.seek(44);

  d.fillScreen(COL_BG);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5);
  d.print("播放中");
  d.drawFastHLine(0, 26, d.width(), COL_DIM);
  d.setTextColor(COL_DIM, COL_BG);
  d.setCursor(10, 34);
  d.print(path);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(10, 58);
  d.print("任意键停止");

  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x32, 0xBF, 400000);  // DAC 0dB(不过推, 给小喇叭留余量, 减回放削波"哒哒")

  do { M5Cardputer.update(); delay(10); } while (M5Cardputer.Keyboard.isPressed());

  bool stop = false;
  uint32_t played = 0, lastDraw = 0, remaining = dataSize;
  int pi = 0;
  while (!stop && remaining > 0) {
    // 先等队列有空位, 再读进"空闲的那块"缓冲, 避免覆盖正在播放的数据
    while (M5Cardputer.Speaker.isPlaying(0) >= 2) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) { stop = true; break; }
      delay(1);
    }
    if (stop) break;
    size_t want = remaining < sizeof(pbBuf[pi]) ? remaining : sizeof(pbBuf[pi]);
    int got = f.read((uint8_t *)pbBuf[pi], want);
    if (got <= 0) break;
    remaining -= got;
    M5Cardputer.Speaker.playRaw(pbBuf[pi], got / 2, REC_RATE, false, 1, 0, false);
    pi ^= 1;
    played += got;
    uint32_t now = millis();
    if (now - lastDraw > 300) {
      lastDraw = now;
      d.fillRect(0, 82, d.width(), 18, COL_BG);
      d.setTextColor(COL_DIM, COL_BG);
      d.setCursor(10, 82);
      d.printf("%lu / %lu KB", (unsigned long)(played / 1024), (unsigned long)(dataSize / 1024));
    }
  }
  if (stop) {
    M5Cardputer.Speaker.stop();
  } else {
    while (M5Cardputer.Speaker.isPlaying(0)) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) { M5Cardputer.Speaker.stop(); break; }
      delay(10);
    }
  }
  f.close();
  SD.end();
}

// ---------- 录音 ----------
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

// 谱减法降噪: 读 path 的 WAV -> 处理 -> 覆盖回 path
void noiseReduce(const char *path) {
  auto &d = M5Cardputer.Display;
  const int N = 256, HOP = 128;
  static float win[256], noiseMag[129], re[256], im[256], frame[256], ola[256];
  static int16_t hopbuf[128];

  d.fillScreen(COL_BG);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5); d.print("降噪处理中...");
  d.drawFastHLine(0, 26, d.width(), COL_DIM);

  if (!sdMount()) { showMsg("降噪", "SD 读取失败", COL_RED); return; }
  File in = SD.open(path, FILE_READ);
  if (!in || in.size() <= 44) { if (in) in.close(); SD.end(); showMsg("降噪", "文件无效", COL_RED); return; }
  uint32_t dataBytes = in.size() - 44;

  for (int i = 0; i < N; i++) win[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / N));
  for (int i = 0; i < 129; i++) noiseMag[i] = 1e30f;
  for (int i = 0; i < N; i++) { frame[i] = 0; ola[i] = 0; }

  // Pass 1: 估计每个频段的噪声底(全程取最小幅度)
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

  // Pass 2: 谱减 + 重建 -> 临时文件
  char tmp[48]; snprintf(tmp, sizeof(tmp), "%s.t", path);
  File out = SD.open(tmp, FILE_WRITE);
  if (!out) { in.close(); SD.end(); showMsg("降噪", "无法写临时文件", COL_RED); return; }
  writeWavHeader(out, REC_RATE, 0);
  for (int i = 0; i < N; i++) { frame[i] = 0; ola[i] = 0; }
  in.seek(44);
  uint32_t outBytes = 0, processed = 0, lastP = 0;
  const float ALPHA = 3.0f, BETA = 0.05f;  // 过减系数 / 谱底(越大降噪越狠, 太大会有水声)
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
      d.fillRect(0, 40, d.width(), 22, COL_BG);
      d.setTextColor(COL_GREEN, COL_BG); d.setCursor(10, 42);
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

// 画到离屏画布(双缓冲, 不闪). 画布代表屏幕 y>=30 的内容区, 故坐标整体减 30
void drawRecCanvas(M5Canvas &cv, uint32_t elapsedMs, bool blink, int16_t *wave) {
  int W = cv.width(), H = cv.height();
  cv.fillScreen(COL_BG);
  // 顶行: 红点 + 正在录音 + 时间
  if (blink) cv.fillCircle(10, 10, 5, COL_RED);
  cv.setFont(&fonts::efontCN_16);
  cv.setTextColor(COL_GREEN, COL_BG);
  cv.setCursor(22, 2);
  cv.print("正在录音");
  uint32_t s = elapsedMs / 1000;
  cv.setTextColor(COL_DIM, COL_BG);
  cv.setCursor(W - 50, 2);
  cv.printf("%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  // 荧光绿声波 (示波器)
  int wfTop = 22, wfBot = H - 14;
  int cy = (wfTop + wfBot) / 2, half = (wfBot - wfTop) / 2;
  cv.drawFastHLine(0, cy, W, COL_DIM);  // 中线
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
  // 底行: 增益 + 提示
  cv.setTextColor(COL_DIM, COL_BG);
  cv.setCursor(6, H - 13);
  cv.printf("音量%d  W+/S-  其他键停", recGain);
}

void recorderApp() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(6, 5);
  d.print("录音机");
  d.drawFastHLine(0, 26, d.width(), COL_DIM);

  if (!sdMount()) { showMsg("录音机", "未检测到 SD 卡", COL_RED); return; }
  if (!SD.exists("/REC")) SD.mkdir("/REC");

  char path[40];
  int idx = 1;
  do { snprintf(path, sizeof(path), "/REC/REC_%04d.wav", idx++); } while (SD.exists(path));

  File f = SD.open(path, FILE_WRITE);
  if (!f) { SD.end(); showMsg("录音机", "无法写入文件", COL_RED); return; }
  writeWavHeader(f, REC_RATE, 0);

  M5Cardputer.Speaker.end();
  // 按官方出厂演示配置麦克风: 高数字增益(库默认 PGA 最小, 靠 magnification 补) + 噪声滤波
  {
    auto mc = M5Cardputer.Mic.config();
    mc.magnification      = 1;   // 原始采集; 主增益走模拟 PGA, 数字只做微调(更干净)
    mc.noise_filter_level = 3;   // 降噪(减少莎莎声)
    M5Cardputer.Mic.config(mc);
  }
  if (!M5Cardputer.Mic.begin()) {
    f.close();
    SD.end();
    M5Cardputer.Speaker.begin();
    showMsg("录音机", "麦克风启动失败", COL_RED);
    return;
  }
  // 短稳定(开机已预热过编解码器, 这里只需让模拟略settle)
  delay(80);
  // 提高 ES8311 模拟增益(PGA): 模拟端放大更干净
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);
  delay(20);

  do { M5Cardputer.update(); delay(10); } while (M5Cardputer.Keyboard.isPressed());

  // 离屏画布(双缓冲, 消除录音界面频闪); 覆盖屏幕 y>=30 的内容区
  M5Canvas cv(&M5Cardputer.Display);
  cv.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height() - 30);

  uint32_t dataBytes = 0;
  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  M5Cardputer.Mic.record(recBuf[1], REC_N, REC_RATE);
  int b = 0;
  uint32_t startMs = millis(), lastDraw = 0;
  bool stop = false;
  uint32_t bufCount = 0;
  int32_t lpf = 0;               // 一阶低通滤波器状态
  const uint32_t HEAD_SKIP = 13;  // 掐头: 跳过开头约 0.2s, 去掉"开始键"声

  while (!stop) {
    while (M5Cardputer.Mic.isRecording() >= 2) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.isKeyPressed('w') || M5Cardputer.Keyboard.isKeyPressed('W')) { if (recGain < 200) recGain += 4; }
        else if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) { if (recGain > 2) recGain -= 4; }
        else { stop = true; break; }
      }
      delay(1);
    }
    if (stop) break;
    int16_t *filled = recBuf[b];
    // 增益 -> 低通(压高频底噪) -> 软限幅(避免硬削波"哒哒"爆裂)
    for (size_t k = 0; k < REC_N; k++) {
      int32_t x = (int32_t)filled[k] * recGain;            // 软件增益
      lpf += (int32_t)(((int64_t)(x - lpf) * 160) >> 8);   // 一阶低通(~2.6kHz)
      float yf = 32767.0f * tanhf((float)lpf / 32767.0f);  // 软限幅: 大声平滑压住而非硬切
      filled[k] = (int16_t)yf;
    }
    bufCount++;
    if (bufCount > HEAD_SKIP) {  // 掐头后才写入
      f.write((uint8_t *)filled, REC_N * sizeof(int16_t));
      dataBytes += REC_N * sizeof(int16_t);
    }
    uint32_t now = millis();
    if (now - lastDraw > 33) {  // 波形刷新(已确认绘制不是"哒哒"来源)
      lastDraw = now;
      bool blink = ((now / 400) % 2) == 0;
      drawRecCanvas(cv, now - startMs, blink, filled);  // 画到离屏画布
      cv.pushSprite(0, 30);                             // 一次性推到屏幕, 不闪
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);  // 画完再重新入队
    b ^= 1;
  }
  cv.deleteSprite();

  M5Cardputer.Mic.end();
  // 掐尾: 申报长度去掉末尾约 0.4s, 去掉"结束键"声
  uint32_t tailTrim = REC_RATE * 8 / 10;  // 12800字节=6400采样=0.4秒
  uint32_t effData = (dataBytes > tailTrim) ? (dataBytes - tailTrim) : 0;
  writeWavHeader(f, REC_RATE, effData);
  f.flush();
  f.close();
  SD.end();
  M5Cardputer.Speaker.begin();
  strncpy(lastRecPath, path, sizeof(lastRecPath));

  // 结果页: P 回放 / 任意键返回
  while (true) {
    d.fillScreen(COL_BG);
    d.setFont(&fonts::efontCN_16);
    d.setTextColor(COL_GREEN, COL_BG);
    d.setCursor(6, 5);
    d.print("已保存");
    d.drawFastHLine(0, 26, d.width(), COL_DIM);
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(10, 34);
    d.print(path);
    d.setTextColor(COL_GREEN, COL_BG);
    d.setCursor(10, 58);
    d.printf("时长 %lu 秒", (unsigned long)((effData / 2) / REC_RATE));
    d.setTextColor(COL_DIM, COL_BG);
    d.setCursor(10, 80);
    d.print("[回车]播放  [N]降噪");
    d.setCursor(10, 100);
    d.print("其他键返回");

    do { M5Cardputer.update(); delay(10); } while (M5Cardputer.Keyboard.isPressed());
    bool play = false, back = false, denoise = false;
    while (!play && !back && !denoise) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.keysState().enter) play = true;
        else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N')) denoise = true;
        else back = true;
      }
      delay(10);
    }
    if (back) break;
    if (denoise) { noiseReduce(path); continue; }  // 降噪后回结果页(文件已更新, 可再按回车对比)
    if (play) playWav(path);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(120);

  int p;
  p = M5.getPin(m5::pin_name_t::sd_spi_sclk); if (p >= 0) sdSCLK = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_cipo); if (p >= 0) sdMISO = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_copi); if (p >= 0) sdMOSI = p;
  p = M5.getPin(m5::pin_name_t::sd_spi_cs);   if (p >= 0) sdCS   = p;

  // 预热麦克风: 开机先完整空跑一次"录音会话"(真的录几帧并丢弃),
  // 消耗掉冷启动后第一次会话的不稳定, 让用户真正第一次录音就正常(否则又小又断续)
  M5Cardputer.Speaker.end();
  {
    auto mc = M5Cardputer.Mic.config();
    mc.magnification = 1;
    mc.noise_filter_level = 3;
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
  M5Cardputer.Mic.end();
  M5Cardputer.Speaker.begin();

  // 预热 SD 卡: 首次写入有延迟, 先暖一下避免第一次录音掉帧/断续
  if (sdMount()) {
    File wf = SD.open("/.warm", FILE_WRITE);
    if (wf) {
      uint8_t z[512] = {0};
      for (int i = 0; i < 16; i++) wf.write(z, sizeof(z));
      wf.close();
      SD.remove("/.warm");
    }
    SD.end();
  }

  drawHome();
}

void loop() {
  M5Cardputer.update();

  uint32_t now = millis();
  if (now - lastBlink > 500) {
    lastBlink = now;
    curOn = !curOn;
    auto &d = M5Cardputer.Display;
    d.setFont(&fonts::efontCN_16);
    d.setTextColor(curOn ? COL_GREEN : COL_BG, COL_BG);
    d.setCursor(10, 92);
    d.print("_");
  }
  if (now - lastBat > 5000) { lastBat = now; drawBattery(); }

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    if (M5Cardputer.Keyboard.isKeyPressed(' ')) {
      recorderApp();
      drawHome();
    } else if (M5Cardputer.Keyboard.keysState().enter) {
      char latest[40];
      if (findLatestRec(latest, sizeof(latest))) {
        strncpy(lastRecPath, latest, sizeof(lastRecPath));
        playWav(latest);
      } else {
        showMsg("播放", "还没有录音", COL_DIM);
      }
      drawHome();
    }
  }
}
