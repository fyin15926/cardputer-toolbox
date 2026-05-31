/*
 * 鎴戠殑宸ュ叿绠?(My Toolbox) for M5Stack Cardputer-Adv
 * 椋庢牸: 绾粦鑳屾櫙 + 榛戝甯濆浗鑽у厜缁?+ 绾挎€?+ 涓枃鐣岄潰 + 妯睆(240x135)
 *
 * 宸ュ叿绠变氦浜?
 *   寮€鏈? 鑷姩寮€濮嬪綍闊? 鎭睆: 绌烘牸=褰曢煶, 鍥炶溅=褰曢煶鍒楄〃, F=鐣寗閽? Alt=搴旂敤鍒楄〃
 *
 * 褰曢煶搴旂敤浜や簰:
 *   褰曞埗涓? 绌烘牸=鏆傚仠/缁х画; 鍥炶溅=缁撴潫(瀛樼洏骞惰繘鍏ュ垪琛?; Esc=瀛樼洏骞舵伅灞? 闀挎寜Del 1.2绉?鍙栨秷骞跺垹闄ゆ湰鏉? W/S=闊抽噺
 *   褰曢煶鍒楄〃: 宸?鍙冲垏 REC/QCK/IMP; ;/. 涓婁笅閫? 鍥炶溅=鎾斁; Esc=鎭睆; 闀挎寜Del=鍒犻櫎; Tab+閿?缁戝畾蹇嵎; Tab+Enter=鏍囬噸瑕? Alt=闄嶅櫔
 *   鍥炴斁: 鎾斁瀹岃嚜鍔ㄥ洖鍒楄〃; 鍥炶溅=鏆傚仠/缁х画; 閫€鏍?鍥炲垪琛? ;/.=涓婁竴鏉?涓嬩竴鏉? Esc=鎭睆; +/- (= 涓?- 閿?=闊抽噺; 闀挎寜Del 1.2绉?鍒犻櫎褰撳墠褰曢煶; 绌烘牸=鍘诲綍闊?
 *   缁戝畾鐨勬挱鏀鹃敭=鏈€楂樹紭鍏堢骇, 浠绘剰鐣岄潰(褰曢煶涓櫎澶?鍗虫寜鍗虫挱, 涓旀挱鏀句腑鎸夊埆鐨勯敭鍙鐩栧垏鎹?
 *
 * 鏂瑰悜閿?鐗╃悊): ; = 涓? . = 涓? , = 宸? / = 鍙?
 */
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <math.h>
#include "esp_sleep.h"   // 杞荤潯鐪?鎭睆鐪佺數)
#include "driver/gpio.h" // 閿洏涓柇鍞ら啋(GPIO11)

#define UPLOAD_WIFI_ENABLED 1  // 闇€瑕佷娇鐢?8MB Flash + 3MB APP 鍒嗗尯缂栬瘧
#if UPLOAD_WIFI_ENABLED
#include <WiFi.h>
#include "esp_wifi.h"
#include <time.h>
#endif

// ---------- 灞忓箷鏂瑰悜 ----------
// 妯睆: 1 鎴?3. 鑻ヤ笂涓嬮鍊? 鏀瑰彟涓€涓嵆鍙?
#define SCREEN_ROT 1

// ---------- 閰嶈壊 ----------
#define COL_BG    0x0000   // 绾粦
#define COL_GREEN 0x07E0   // 鑽у厜缁?浜?
#define COL_DIM   0x0320   // 鏆楃豢
#define COL_RED   0xF800   // 褰曢煶绾㈢偣
#define COL_WHITE 0xFFFF   // 鎾斁澶寸櫧绾?
#define COL_GRAY  0x7BEF
#define COL_DARK_GRAY 0x2104

static const uint8_t REC_BUILTIN_ACCENT_COUNT = 6;
static const uint8_t REC_CUSTOM_ACCENT_MAX = 6;
static const uint8_t REC_ACCENT_CAPACITY = REC_BUILTIN_ACCENT_COUNT + REC_CUSTOM_ACCENT_MAX;
static uint16_t REC_ACCENT_COLORS[REC_ACCENT_CAPACITY] = {
  0x07E0, // green
  0x07FF, // cyan
  0x04FF, // sky
  0xF81F, // magenta
  0xFFE0, // yellow
  0xFD20  // amber
};
static uint8_t recAccentIndex = 0;
static uint8_t recAccentCount = REC_BUILTIN_ACCENT_COUNT;

struct WavInfo {
  uint32_t sampleRate = 0;
  uint32_t dataOffset = 0;
  uint32_t dataBytes = 0;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  uint16_t blockAlign = 0;
  const char *error = "BAD WAV";
};

static uint16_t recScaleColor(uint16_t col, uint8_t num, uint8_t den, uint16_t minR = 0, uint16_t minG = 0, uint16_t minB = 0) {
  uint16_t r = (col >> 11) & 0x1f;
  uint16_t g = (col >> 5) & 0x3f;
  uint16_t b = col & 0x1f;
  r = (uint16_t)((r * num) / den);
  g = (uint16_t)((g * num) / den);
  b = (uint16_t)((b * num) / den);
  if (r < minR) r = minR;
  if (g < minG) g = minG;
  if (b < minB) b = minB;
  return (r << 11) | (g << 5) | b;
}

static uint16_t recAccentColor() {
  if (recAccentCount == 0) recAccentCount = REC_BUILTIN_ACCENT_COUNT;
  return REC_ACCENT_COLORS[recAccentIndex % recAccentCount];
}

static uint16_t recDimColor() {
  return recScaleColor(recAccentColor(), 2, 5, 1, 2, 1);
}

static uint16_t recAxisColor() {
  return recScaleColor(recAccentColor(), 1, 5, 0, 1, 0);
}

static uint16_t recSelectBgColor() {
  return recScaleColor(recAccentColor(), 1, 6);
}

static void cycleRecAccent(int delta) {
  const uint8_t n = recAccentCount > 0 ? recAccentCount : REC_BUILTIN_ACCENT_COUNT;
  recAccentIndex = (recAccentIndex + n + delta) % n;
}

static bool saveRecAccentStateMounted();
static void saveRecAccentState() {
  if (!sdMount()) return;
  saveRecAccentStateMounted();
  SD.end();
}

// ---------- 鍏ㄥ睆 UI 甯冨眬甯搁噺 ----------
// 灞忓箷 240脳135; 涓嶅啀缁樺埗鍙充晶鏍囩鏍? 鎵€鏈夌晫闈娇鐢ㄥ叏瀹藉唴瀹瑰尯
#define CONTENT_W  240     // 鍐呭鍖哄搴?(x: 0..239)
#define WAVE_TOP    20     // 娉㈠舰鍖洪《 y
#define WAVE_BOT   109     // 娉㈠舰鍖哄簳 y  (浣?26px 缁欒鏃跺櫒)
#define WAVE_H      89     // 娉㈠舰鍖洪珮搴?
#define WAVE_CY     50     // A/B 鍒嗙晫: 绾?2:1, B绾夸綔涓轰富瑙嗚
#define CHROME_TOP  (WAVE_BOT + 1)
#define CHROME_H    (135 - CHROME_TOP)
static const int A_CY   = (WAVE_TOP + WAVE_CY) / 2;        // A绾?杞ㄩ亾绾夸腑杞?
static const int A_HALF = (WAVE_CY - WAVE_TOP) / 2 - 2;    // A绾挎渶澶у崐骞?
static const int B_CY   = (WAVE_CY + WAVE_BOT) / 2;        // B绾?鐩戝惉绾夸腑杞?
static const int B_HALF = (WAVE_BOT - WAVE_CY) / 2 - 2;    // B绾挎渶澶у崐骞?
static const uint8_t REC_B_SMOOTH = 2;   // 褰曢煶 B绾垮钩婊? 瓒婂ぇ瓒婄ǔ, 瓒婂皬瓒婄伒鏁?
static const uint8_t PB_B_SMOOTH  = REC_B_SMOOTH;   // 鎾斁 B绾胯窡褰曢煶椤典竴鑷?
static const uint8_t B_DECAY      = 5;   // 鏃犳柊闊抽鏃跺洖涓嚎閫熷害: 瓒婂皬鍥炶惤瓒婂揩
static const int32_t A_RMS_FULL   = 18000; // A绾挎弧鏍奸槇鍊? 瓒婂ぇ瓒婁笉鏁忔劅

// ---------- 褰曢煶鍙傛暟 ----------
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
static const uint32_t SHORTCUT_RATE_MIN = 2000;
static const uint32_t SHORTCUT_RATE_MAX = 48000;
static const int32_t  SHORTCUT_TRIM_RMS = 900;   // 蹇嵎闊抽鍒囧ご鍘诲熬闈欓煶闃堝€?
static const uint32_t SHORTCUT_HEAD_PAD = REC_RATE / 50;  // 20ms, 闃叉寮€澶磋鍒囩‖
static const uint32_t SHORTCUT_TAIL_PAD = REC_RATE / 10;  // 100ms, 鐣欎竴鐐硅嚜鐒舵敹灏?
static const int32_t  SHORTCUT_RETRIM_RMS = 1600;
static const uint32_t SHORTCUT_RETRIM_HEAD_PAD = REC_RATE / 200; // 5ms
static const uint32_t SHORTCUT_RETRIM_TAIL_PAD = REC_RATE / 40;  // 25ms
static const int32_t  SHORTCUT_SUPERTRIM_RMS = 2200;
static const uint32_t SHORTCUT_SUPERTRIM_HEAD_PAD = REC_RATE / 1000; // 1ms
static const uint32_t SHORTCUT_SUPERTRIM_TAIL_PAD = REC_RATE * 12 / 1000; // 12ms
static const uint32_t SHORTCUT_FADE_SAMPLES = REC_RATE / 125; // 8ms 娣″叆娣″嚭
static const size_t   REC_N    = 256;    // 姣忕紦鍐叉牱鏈暟 (~16ms, 灏?娉㈠舰鏇存祦鐣?
static const size_t   PB_N     = 2048;   // Playback int16 samples; larger buffer reduces SD/UI underruns.
static const uint32_t REC_UI_FRAME_MS = 33;  // Sprite path: about 30fps while keeping audio/SD priority.
static const uint32_t PB_UI_FRAME_MS  = 33;  // Sprite path: smoother playback UI without pushing toward 60fps.
static const uint32_t DIRECT_UI_FRAME_MS = 66;  // No-sprite fallback: about 15fps to avoid direct-draw flicker.
static const uint32_t REC_AUTO_SEGMENT_MS = 30UL * 60UL * 1000UL;
static const uint32_t ROLLBACK_KEEP_MS = 120UL * 1000UL;
static const uint32_t ROLLBACK_KEEP_BYTES = REC_RATE * 2UL * (ROLLBACK_KEEP_MS / 1000UL);
static const uint8_t  REC_WRITE_BATCH = 4;
static const uint8_t  REC_REARM_SETTLE_BUFFERS = 6;
static const uint32_t UPLOAD_IDLE_DELAY_MS = 3000;
static const uint32_t SPEAKER_IDLE_OFF_MS = 3000;
static const uint32_t PLAYBACK_EDGE_FADE_BYTES = REC_RATE * 2 * 80 / 1000;
static const uint32_t HOTKEY_END_HOLD_MS = 3000;
static int16_t recBuf[2][REC_N];
static int16_t pbBuf[2][PB_N];           // 鎾斁鍙岀紦鍐?鏀句竴鍧?璇诲彟涓€鍧? 閬垮厤瑕嗙洊鐮撮煶)
static int16_t recWriteBuf[REC_WRITE_BATCH * REC_N];
static int16_t recVisualBuf[REC_N];
static int16_t playbackVisualBuf[PB_N];
static int16_t speakerSilenceBuf[PB_N];
static int16_t seekToneBuf[384];
int recGain = 36;                        // 褰曢煶杞欢澧炵泭榛樿鍊?W/S 鐜板満鍙皟, 甯﹀墛娉繚鎶?
int playVol = 200;                       // 鍥炴斁闊抽噺(+/- 鍙皟, 0..255)
static char g_shortcutKeyRoot = 'C';
static int8_t g_shortcutTransposeSemis = 0;
static bool g_scaleMode = false;
static int g_scaleCurrentRec = 0;
static float g_shortcutSemisOverride = 127.0f;
static int g_scalePitchCacheRec = 0;
static float g_scalePitchCacheHz = 0.0f;
static bool g_perfHudVisible = false;
static char g_perfHudLastKey = 0;
static char g_perfHudPendingKey = 0;
static uint8_t g_perfHudLastKind = 255;
static float g_shortcutBendNeutralX = 0.0f;
static float g_shortcutVibNeutralY = 0.0f;
static float g_shortcutBendSemis = 0.0f;
static float g_shortcutVibDepthSemis = 0.0f;
static float g_shortcutTailGain = 1.0f;
static uint32_t g_shortcutVibStartMs = 0;
static const int VOL_LEVELS = 10;
static const uint8_t VOLUME_VALUES[VOL_LEVELS + 1] = {0, 42, 58, 76, 98, 122, 150, 180, 210, 235, 255};
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
static bool g_uploadModeActive = false;
static bool g_uploadModeVisible = false;
static bool g_uploadModeIgnoreKeys = false;
static bool g_uploadExitRequested = false;
static uint8_t g_uploadExitApp = 0;
static uint8_t g_uploadBatchDone = 0;
static uint8_t g_uploadBatchTotal = 0;
static bool g_wifiSessionActive = false;
static bool g_mediaBusy = false;

// SD 寮曡剼(杩愯鏃剁敱 M5Unified 鎸夋満鍨嬬粰鍑? 澶辫触鍒欏洖閫€鍒?Adv 宸茬煡鍊?
int sdSCLK = 40, sdMISO = 39, sdMOSI = 14, sdCS = 12;

static const char *REC_DIR = "/REC";
static const char *SHORTCUT_DIR = "/SHORTCUT";
static const char *IMPORTANT_DIR = "/IMPORTANT";
static const char *MUSIC_DIR = "/MUSIC";
static const char *CHAT_DIR = "/CHAT";
static const char *CHAT_LAST_PATH = "/CHAT/CHAT_LAST.wav";
static const char *CHAT_REPLY_PATH = "/CHAT/CHAT_REPLY.wav";
static const char *HOTKEY_PATH = "/SHORTCUT/keys.txt";
static const char *HOTKEY_GROUP_PATHS[3] = {"/SHORTCUT/keys_1.txt", "/SHORTCUT/keys_2.txt", "/SHORTCUT/keys_3.txt"};
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
static const char *REC_COLOR_PATH = "/REC/color.txt";
static const char *ROLLBACK_TMP_PATH = "/REC/.rollback.pcm";
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

enum RecKind : uint8_t { REC_NORMAL = 0, REC_SHORTCUT = 1, REC_IMPORTANT = 2, REC_MUSIC = 3 };
static const uint8_t REC_KIND_COUNT = 4;
static const uint16_t MAX_MUSIC_TRACKS = 160;
static const int MUSIC_ID_BASE = MAX_REC - MAX_MUSIC_TRACKS + 1;
static char musicPaths[MAX_MUSIC_TRACKS][128];
static uint16_t musicTrackCount = 0;
static const uint8_t NORMAL_INITIAL_LOAD = 5;
static const uint8_t NORMAL_OLDER_LOAD = 20;
static int normalOldestLoaded = 0;
static bool normalReachedBeginning = false;

static void loadRecAccentPresetMounted() {
  File f = SD.open(REC_COLOR_PATH, FILE_READ);
  if (!f) return;
  char line[32];
  bool loadedNewFormat = false;
  uint8_t loadedIndex = recAccentIndex;
  recAccentCount = REC_BUILTIN_ACCENT_COUNT;
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    if (strncmp(p, "IDX ", 4) == 0) {
      int idx = atoi(p + 4);
      if (idx >= 0 && idx < (int)REC_ACCENT_CAPACITY) {
        loadedIndex = (uint8_t)idx;
        loadedNewFormat = true;
      }
    } else if (strncmp(p, "CUSTOM ", 7) == 0) {
      uint32_t rgb = 0;
      if (parseHexRgb(p + 7, rgb)) {
        addRecAccentPreset(rgb888To565(rgb));
        loadedNewFormat = true;
      }
    } else {
      uint32_t rgb = 0;
      if (parseHexRgb(p, rgb)) {
        loadedIndex = addRecAccentPreset(rgb888To565(rgb));
        loadedNewFormat = true;
      }
    }
  }
  f.close();
  if (loadedNewFormat) {
    recAccentIndex = loadedIndex < recAccentCount ? loadedIndex : (recAccentCount - 1);
  }
}

static bool saveRecAccentStateMounted() {
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  const char *tmp = "/REC/color.tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  f.printf("IDX %u\n", (unsigned)recAccentIndex);
  for (uint8_t i = REC_BUILTIN_ACCENT_COUNT; i < recAccentCount; i++) {
    uint32_t rgb = rgb565To888(REC_ACCENT_COLORS[i]);
    f.printf("CUSTOM #%02X%02X%02X\n", (unsigned)((rgb >> 16) & 0xff), (unsigned)((rgb >> 8) & 0xff), (unsigned)(rgb & 0xff));
  }
  f.close();
  SD.remove(REC_COLOR_PATH);
  bool ok = SD.rename(tmp, REC_COLOR_PATH);
  if (!ok) SD.remove(tmp);
  return ok;
}

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
  if (kind == REC_MUSIC && recNum >= MUSIC_ID_BASE && recNum <= MAX_REC) {
    uint16_t slot = (uint16_t)(recNum - MUSIC_ID_BASE);
    if (slot < musicTrackCount && musicPaths[slot][0]) {
      snprintf(p, n, "%s", musicPaths[slot]);
      return;
    }
  }
  const char *dir = (kind == REC_SHORTCUT) ? SHORTCUT_DIR : ((kind == REC_IMPORTANT) ? IMPORTANT_DIR : ((kind == REC_MUSIC) ? MUSIC_DIR : REC_DIR));
  snprintf(p, n, "%s/REC_%04d.wav", dir, recNum);
}

static void recordingPath(int recNum, bool important, char *p, size_t n) {
  recordingPathKind(recNum, important ? REC_IMPORTANT : REC_NORMAL, p, n);
}

// ---------- 蹇嵎閿粦瀹?(瀛楁瘝閿?-> 褰曢煶缂栧彿), 瀛樺埌 SD 鍗?/SHORTCUT/keys.txt ----------
struct HotKey { char key; int idx; };
static const int MAX_HOTKEY = 36;
static const uint8_t HOTKEY_GROUPS = 3;
HotKey hotkeys[HOTKEY_GROUPS][MAX_HOTKEY];
int hotkeyCount[HOTKEY_GROUPS] = {0, 0, 0};
static uint8_t hotkeyGroup = 0;
static bool hotkeysLoaded = false;

static bool micInputReady = false;
static bool forceMicRearm = true;  // 寮€鏈?鍞ら啋鍚庣殑绗竴娆℃寮忓綍闊宠寮哄埗閲嶅缓杈撳叆閾捐矾
static bool autoRecordPending = true;
static bool speakerOutputReady = false;
static int speakerVolumeCurrent = 0;
static uint32_t speakerIdleOffAtMs = 0;
static bool speakerDrainBeforeNextPlayback = false;
static bool speakerColdStarted = false;

#define APP_SLEEP      0
#define APP_REC_RECORD 1
#define APP_REC_LIST   2
#define APP_LAUNCHER   3
#define APP_POMODORO   4
#define APP_WIFI       5
#define APP_CHAT       6
#define APP_ROLLBACK   7

static uint8_t wakeApp = APP_REC_LIST;

// ---------- SD 杈呭姪 ----------
static bool sdMount() {
  SPI.begin(sdSCLK, sdMISO, sdMOSI, sdCS);
  return SD.begin(sdCS, SPI, 25000000);
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool parseHexRgb(const char *text, uint32_t &rgb) {
  if (!text) return false;
  if (*text == '#') text++;
  if (strlen(text) != 6) return false;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    int h = hexValue(text[i]);
    if (h < 0) return false;
    v = (v << 4) | (uint32_t)h;
  }
  rgb = v;
  return true;
}

static uint16_t rgb888To565(uint32_t rgb) {
  uint16_t r = (uint16_t)((rgb >> 19) & 0x1f);
  uint16_t g = (uint16_t)((rgb >> 10) & 0x3f);
  uint16_t b = (uint16_t)((rgb >> 3) & 0x1f);
  return (r << 11) | (g << 5) | b;
}

static uint32_t rgb565To888(uint16_t col) {
  uint32_t r = (col >> 11) & 0x1f;
  uint32_t g = (col >> 5) & 0x3f;
  uint32_t b = col & 0x1f;
  r = (r * 255 + 15) / 31;
  g = (g * 255 + 31) / 63;
  b = (b * 255 + 15) / 31;
  return (r << 16) | (g << 8) | b;
}

static uint8_t addRecAccentPreset(uint16_t col) {
  for (uint8_t i = 0; i < recAccentCount; i++) {
    if (REC_ACCENT_COLORS[i] == col) return i;
  }
  if (recAccentCount < REC_ACCENT_CAPACITY) {
    REC_ACCENT_COLORS[recAccentCount] = col;
    return recAccentCount++;
  }
  REC_ACCENT_COLORS[REC_ACCENT_CAPACITY - 1] = col;
  return REC_ACCENT_CAPACITY - 1;
}

static void setRecCustomAccent(uint16_t col) {
  recAccentIndex = addRecAccentPreset(col);
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

static bool isWavFileName(const char *name) {
  if (!name) return false;
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  if (!base[0] || base[0] == '.') return false;
  const char *dot = strrchr(base, '.');
  if (!dot) return false;
  return (dot[1] == 'w' || dot[1] == 'W') &&
         (dot[2] == 'a' || dot[2] == 'A') &&
         (dot[3] == 'v' || dot[3] == 'V') &&
         dot[4] == '\0';
}

static int musicRecIdForSlot(uint16_t slot) {
  return (slot < MAX_MUSIC_TRACKS) ? (MUSIC_ID_BASE + (int)slot) : 0;
}

static int musicSlotForRec(int recNum) {
  if (recNum < MUSIC_ID_BASE || recNum > MAX_REC) return -1;
  int slot = recNum - MUSIC_ID_BASE;
  return (slot >= 0 && slot < (int)musicTrackCount) ? slot : -1;
}

static void resetMusicTracks() {
  musicTrackCount = 0;
  memset(musicPaths, 0, sizeof(musicPaths));
}

static void musicListLabel(int recNum, char *out, size_t n) {
  int slot = musicSlotForRec(recNum);
  if (slot < 0) {
    snprintf(out, n, "%04d", recNum);
    return;
  }
  snprintf(out, n, "MU%02d", slot + 1);
}

static void musicDisplayName(int recNum, char *out, size_t n) {
  if (n == 0) return;
  out[0] = 0;
  int slot = musicSlotForRec(recNum);
  if (slot < 0 || slot >= (int)musicTrackCount) {
    musicListLabel(recNum, out, n);
    return;
  }
  const char *base = strrchr(musicPaths[slot], '/');
  base = base ? base + 1 : musicPaths[slot];
  snprintf(out, n, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

// ---------- WAV 澶?----------
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
  wr16(f, 1);          // 鍗曞０閬?
  wr32(f, rate);
  wr32(f, rate * 2);   // byteRate
  wr16(f, 2);          // blockAlign
  wr16(f, 16);         // bits
  f.write((const uint8_t *)"data", 4);
  wr32(f, dataBytes);
}

// ---------- 鐣岄潰杩斿洖鐮?----------
#define R_BACK   0   // 閫€鏍? 杩斿洖涓婁竴灞?
#define R_RECORD 1   // 绌烘牸: 鍘诲紑濮嬪綍闊?
#define R_PLAY   2   // 鎸変笅鏌愪釜宸茬粦瀹氱殑鎾斁閿? 绔嬪埢鍘绘挱鏀?鍙鐩栧綋鍓嶆挱鏀?
#define R_LIST   3   // 鍒楄〃閿? 杩涘叆褰曢煶鍒楄〃
#define R_NOISE  4   // Alt: 瀵规渶杩?褰撳墠褰曢煶闄嶅櫔
#define R_DELETE 5   // Del long-press delete
#define R_ROLLBACK 6 // Tab rollback mode
int g_nextPlay = 0;  // 閰嶅悎 R_PLAY: 瑕佸垏鎹㈠幓鎾斁鐨勫綍闊崇紪鍙?
int g_afterRecord = R_LIST;  // 褰曢煶缁撴潫鍚庤烦杞洰鏍?
static bool g_uploadAfterRecord = false;
static bool g_seamlessHotkeySwitch = false;
int g_listReturnRec = 0;     // 鎾斁椤佃繑鍥炲垪琛ㄦ椂搴旈噸鏂伴€変腑鐨勫綍闊?
int g_carryDeleteRec = 0;
uint32_t g_carryDeleteStart = 0;
uint8_t g_listMode = REC_NORMAL;  // 0 normal / 1 quick / 2 important / 3 music
static int g_listModeSelectedRec[REC_KIND_COUNT] = {0, 0, 0, 0};
static int nextRecHint = 0;  // 涓嬩竴涓綍闊崇紪鍙风紦瀛? 閬垮厤姣忔浠?REC_0001 椤哄簭鎺㈡祴
static const uint32_t DELETE_HOLD_MS = 1200;
static const uint32_t DELETE_HINT_MS = 280;  // 鐭寜 Del 涓嶆樉绀哄垹闄ゆ彁绀? 闀挎寜鍚庢墠鍑虹幇

// ---------- 鎸夐敭灏忓伐鍏?----------
// 鍙栧綋鍓嶆寜涓嬬殑绗竴涓彲缁戝畾閿?瀛楁瘝杞皬鍐? 鎴栨暟瀛?; 娌℃湁鍒欒繑鍥?0
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
static char dispKey(char k) { return (k >= 'a' && k <= 'z') ? (k - 32) : k; }  // 鏄剧ず鐢?瀛楁瘝杞ぇ鍐?
static char pressedBindKeyExcept(char ignored) {
  ignored = normalizeBindKey(ignored);
  for (char c = '0'; c <= '9'; c++) {
    if (c != ignored && M5Cardputer.Keyboard.isKeyPressed(c)) return c;
  }
  const char shiftedDigits[] = ")!@#$%^&*(";
  for (int i = 0; i < 10; i++) {
    char normalized = (char)('0' + i);
    if (normalized != ignored && M5Cardputer.Keyboard.isKeyPressed(shiftedDigits[i])) return normalized;
  }
  for (char c = 'a'; c <= 'z'; c++) {
    if (c == ignored) continue;
    if (M5Cardputer.Keyboard.isKeyPressed(c)) return c;
    if (M5Cardputer.Keyboard.isKeyPressed((char)(c - 32))) return c;
  }
  for (char c : M5Cardputer.Keyboard.keysState().word) {
    char normalized = normalizeBindKey(c);
    if (normalized && normalized != ignored) return normalized;
  }
  return 0;
}

static bool keyCtrl()  { return M5Cardputer.Keyboard.keysState().ctrl; }
static bool keyAlt()   { return M5Cardputer.Keyboard.keysState().alt; }
static bool keyFn()    { return M5Cardputer.Keyboard.keysState().fn; }
static bool keyGo()    { return M5Cardputer.BtnA.isPressed(); }
static bool keySpace() { return M5Cardputer.Keyboard.isKeyPressed(' '); }
static bool keyEsc()   { return M5Cardputer.Keyboard.isKeyPressed('`'); }  // Cardputer 鐗╃悊 Esc 閿槧灏勪负宸︿笂瑙?`
static bool keyDel()   { return M5Cardputer.Keyboard.keysState().del; }
static bool keyEnter() { return M5Cardputer.Keyboard.keysState().enter; }
static bool keyTab()   { return M5Cardputer.Keyboard.keysState().tab; }
static bool keyRollback() { return keyTab(); }
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

static bool pressedRootKey(char c) {
  return M5Cardputer.Keyboard.isKeyPressed(c) || M5Cardputer.Keyboard.isKeyPressed((char)(c + 32));
}

static int8_t semisForRoot(char root) {
  switch (root) {
    case 'A': return -3;
    case 'B': return -1;
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    default: return 0;
  }
}

static bool setShortcutKeyRoot(char root) {
  if (root < 'A' || root > 'G') return false;
  g_shortcutKeyRoot = root;
  g_shortcutTransposeSemis = semisForRoot(root);
  return true;
}

static uint32_t shortcutRateForSemis(uint32_t baseRate, float semis) {
  float scale = powf(2.0f, semis / 12.0f);
  uint32_t rate = (uint32_t)((float)baseRate * scale + 0.5f);
  return max(SHORTCUT_RATE_MIN, min(SHORTCUT_RATE_MAX, rate));
}

static float activeShortcutSemis() {
  return (g_shortcutSemisOverride < 126.5f) ? g_shortcutSemisOverride : (float)g_shortcutTransposeSemis;
}

static void clearShortcutSemisOverride() {
  g_shortcutSemisOverride = 127.0f;
}

static bool prepareScaleNoteForRec(int recNum, int8_t targetSemis);

static bool handleShortcutKeyRootChord() {
  if (!keyCtrl()) return false;
  const char roots[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G'};
  for (char root : roots) {
    if (pressedRootKey(root)) return setShortcutKeyRoot(root);
  }
  return false;
}

static const char *shortcutKeyRootLabel() {
  static char label[6];
  snprintf(label, sizeof(label), "KEY:%c", g_shortcutKeyRoot);
  return label;
}

static const char *shortcutGroupLabel() {
  static char label[4];
  snprintf(label, sizeof(label), "G%d", (int)hotkeyGroup + 1);
  return label;
}

static bool setShortcutGroup(uint8_t group) {
  if (group >= HOTKEY_GROUPS) return false;
  hotkeyGroup = group;
  return true;
}

static bool handleShortcutGroupChord() {
  if (!keyCtrl()) return false;
  if (M5Cardputer.Keyboard.isKeyPressed('1') || M5Cardputer.Keyboard.isKeyPressed('!')) { g_scaleMode = false; clearShortcutSemisOverride(); return setShortcutGroup(0); }
  if (M5Cardputer.Keyboard.isKeyPressed('2') || M5Cardputer.Keyboard.isKeyPressed('@')) { g_scaleMode = false; clearShortcutSemisOverride(); return setShortcutGroup(1); }
  if (M5Cardputer.Keyboard.isKeyPressed('3') || M5Cardputer.Keyboard.isKeyPressed('#')) { g_scaleMode = false; clearShortcutSemisOverride(); return setShortcutGroup(2); }
  return false;
}

static bool handleScaleModeChord() {
  if (!keyCtrl()) return false;
  if (!M5Cardputer.Keyboard.isKeyPressed('0') && !M5Cardputer.Keyboard.isKeyPressed(')')) return false;
  g_scaleMode = true;
  clearShortcutSemisOverride();
  return true;
}

static bool scaleNoteSemis(int8_t &semis) {
  const char *midKeys = "1234567";
  const int8_t midSemis[] = {0, 2, 4, 5, 7, 9, 11};
  for (uint8_t i = 0; i < 7; i++) {
    if (M5Cardputer.Keyboard.isKeyPressed(midKeys[i])) { semis = midSemis[i]; return true; }
  }
  const char *hiKeys = "890iop";
  const int8_t hiSemis[] = {12, 14, 16, 17, 19, 21};
  for (uint8_t i = 0; i < 6; i++) {
    char k = hiKeys[i];
    if (M5Cardputer.Keyboard.isKeyPressed(k) || (k >= 'a' && k <= 'z' && M5Cardputer.Keyboard.isKeyPressed((char)(k - 32)))) {
      semis = hiSemis[i];
      return true;
    }
  }
  const char *lowKeys = "qwertyu";
  const int8_t lowSemis[] = {-12, -10, -8, -7, -5, -3, -1};
  for (uint8_t i = 0; i < 7; i++) {
    char k = lowKeys[i];
    if (M5Cardputer.Keyboard.isKeyPressed(k) || M5Cardputer.Keyboard.isKeyPressed((char)(k - 32))) {
      semis = lowSemis[i];
      return true;
    }
  }
  return false;
}

static bool scaleNoteSemisForKey(char k, int8_t &semis) {
  k = normalizeBindKey(k);
  switch (k) {
    case 'q': semis = -12; return true;
    case 'w': semis = -10; return true;
    case 'e': semis = -8; return true;
    case 'r': semis = -7; return true;
    case 't': semis = -5; return true;
    case 'y': semis = -3; return true;
    case 'u': semis = -1; return true;
    case '1': semis = 0; return true;
    case '2': semis = 2; return true;
    case '3': semis = 4; return true;
    case '4': semis = 5; return true;
    case '5': semis = 7; return true;
    case '6': semis = 9; return true;
    case '7': semis = 11; return true;
    case '8': semis = 12; return true;
    case '9': semis = 14; return true;
    case '0': semis = 16; return true;
    case 'i': semis = 17; return true;
    case 'o': semis = 19; return true;
    case 'p': semis = 21; return true;
    default: return false;
  }
}

static bool isScaleSampleKey(char k) {
  return strchr("asdfghjklzxcvbnm", k) != nullptr;
}

static bool scaleModeCanHandleKey() {
  if (!g_scaleMode || keyCtrl() || keyTab() || keyFn() || keyAlt()) return false;
  if (keyEnter() || keyDel() || keySpace() || keyEsc() || keyUpload()) return false;
  if (keyVolUp() || keyVolDn() || keyBrightUp() || keyBrightDn()) return false;
  if (keyUp() || keyDown() || keyLeft() || keyRight()) return false;
  return true;
}

static uint32_t shortcutPlaybackRate(uint32_t baseRate, bool isHotkeyPlayback) {
  float semis = activeShortcutSemis();
  if (!isHotkeyPlayback || semis == 0.0f) return baseRate;
  return shortcutRateForSemis(baseRate, semis);
}

static bool beginShortcutPitchBend(bool isHotkeyPlayback) {
  g_shortcutBendSemis = 0.0f;
  g_shortcutVibDepthSemis = 0.0f;
  g_shortcutTailGain = 1.0f;
  g_shortcutVibStartMs = millis();
  if (!isHotkeyPlayback || !M5.Imu.isEnabled()) return false;
  M5.Imu.update();
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  g_shortcutBendNeutralX = ax;
  g_shortcutVibNeutralY = ay;
  return true;
}

static uint32_t shortcutMotionRate(uint32_t baseRate, bool enabled) {
  if (!enabled) return baseRate;
  M5.Imu.update();
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return baseRate;

  static const float DEADZONE_G = 0.06f;
  static const float FULL_BEND_G = 0.62f;
  static const float MAX_BEND_SEMIS = 1.35f;
  float delta = ax - g_shortcutBendNeutralX;
  float mag = fabsf(delta);
  float targetSemis = 0.0f;
  if (mag > DEADZONE_G) {
    float bend = (mag - DEADZONE_G) / (FULL_BEND_G - DEADZONE_G);
    if (bend > 1.0f) bend = 1.0f;
    targetSemis = (delta < 0.0f ? bend : -bend) * MAX_BEND_SEMIS;
  }
  g_shortcutBendSemis += (targetSemis - g_shortcutBendSemis) * 0.10f;
  if (fabsf(g_shortcutBendSemis) < 0.02f) g_shortcutBendSemis = 0.0f;

  static const float VIB_DEADZONE_G = 0.06f;
  static const float VIB_FULL_G = 0.42f;
  static const float MAX_VIB_DEPTH_SEMIS = 0.26f;
  float forward = g_shortcutVibNeutralY - ay;
  float backward = ay - g_shortcutVibNeutralY;
  float targetVibDepth = 0.0f;
  if (forward > VIB_DEADZONE_G) {
    targetVibDepth = (forward - VIB_DEADZONE_G) / (VIB_FULL_G - VIB_DEADZONE_G);
    if (targetVibDepth > 1.0f) targetVibDepth = 1.0f;
    targetVibDepth *= MAX_VIB_DEPTH_SEMIS;
  }
  g_shortcutVibDepthSemis += (targetVibDepth - g_shortcutVibDepthSemis) * 0.08f;
  if (g_shortcutVibDepthSemis < 0.015f) g_shortcutVibDepthSemis = 0.0f;

  static const float TAIL_DEADZONE_G = 0.06f;
  static const float TAIL_FULL_G = 0.46f;
  static const float MIN_TAIL_GAIN = 0.08f;
  float targetTailGain = 1.0f;
  if (backward > TAIL_DEADZONE_G) {
    float fade = (backward - TAIL_DEADZONE_G) / (TAIL_FULL_G - TAIL_DEADZONE_G);
    if (fade > 1.0f) fade = 1.0f;
    targetTailGain = 1.0f - fade * (1.0f - MIN_TAIL_GAIN);
  }
  g_shortcutTailGain += (targetTailGain - g_shortcutTailGain) * 0.12f;

  uint32_t vibAgeMs = millis() - g_shortcutVibStartMs;
  static const float VIB_RATE_HZ = 4.1f;
  float vibPhase = ((float)vibAgeMs * (VIB_RATE_HZ * 6.2831853f)) / 1000.0f;
  float vibSemis = sinf(vibPhase) * g_shortcutVibDepthSemis;
  float wowPhase = ((float)vibAgeMs * (0.55f * 6.2831853f)) / 1000.0f;
  vibSemis += sinf(wowPhase) * 0.045f;
  float scale = powf(2.0f, (g_shortcutBendSemis + vibSemis) / 12.0f);
  uint32_t rate = (uint32_t)((float)baseRate * scale + 0.5f);
  return max(SHORTCUT_RATE_MIN, min(SHORTCUT_RATE_MAX, rate));
}

static const char *uploadStatusLabel(uint8_t status);
static void drawUploadMode(const char *status, uint16_t col);
static bool keyUploadAbort() {
  bool anyPressed = M5Cardputer.Keyboard.isPressed() || keyGo();
  if (!anyPressed) {
    g_uploadModeIgnoreKeys = false;
    return false;
  }
  if (g_uploadModeActive) {
    bool wakePressed = keySpace() || keyEnter() ||
                       M5Cardputer.Keyboard.isKeyPressed('f') || M5Cardputer.Keyboard.isKeyPressed('F') ||
                       keyChat() || keyGo();
    if (!g_uploadModeVisible) {
      if (!wakePressed) return false;
      applyBrightness();
      g_uploadModeVisible = true;
      g_uploadModeIgnoreKeys = true;
      drawUploadMode(uploadStatusLabel(g_uploadStatus), COL_GREEN);
      return false;
    }
    if (g_uploadModeIgnoreKeys) return false;
    if (keySpace()) {
      g_uploadExitRequested = true;
      g_uploadExitApp = APP_REC_RECORD;
      g_uploadPausedForInput = true;
      return true;
    }
    if (keyEnter()) {
      g_uploadExitRequested = true;
      g_uploadExitApp = APP_REC_LIST;
      g_uploadPausedForInput = true;
      return true;
    }
    if (keyEsc()) {
      g_uploadExitRequested = true;
      g_uploadExitApp = APP_SLEEP;
      g_uploadPausedForInput = true;
      return true;
    }
    return false;
  }
  g_uploadPausedForInput = true;
  if (keyFn() && keyUpload()) g_uploadCancelRequested = true;
  return true;
}

static bool wakeAppFromPressedKeys(uint8_t &app) {
  if (keyRollback()) {
    app = APP_ROLLBACK;
    return true;
  }
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

static int globalVolumeLevel();

static void adjustPlayVolume(int delta) {
  int level = globalVolumeLevel();
  if (delta > 0) level++;
  else if (delta < 0) level--;
  level = max(0, min(VOL_LEVELS, level));
  playVol = VOLUME_VALUES[level];
  if (speakerOutputReady) {
    speakerVolumeCurrent = playVol;
    M5Cardputer.Speaker.setVolume(speakerVolumeCurrent);
  }
}

static int globalVolumeLevel() {
  int best = 0;
  int bestDiff = abs(playVol - (int)VOLUME_VALUES[0]);
  for (int i = 1; i <= VOL_LEVELS; i++) {
    int diff = abs(playVol - (int)VOLUME_VALUES[i]);
    if (diff < bestDiff) {
      best = i;
      bestDiff = diff;
    }
  }
  return best;
}

static int volumeFromLevel(int level) {
  level = max(0, min(VOL_LEVELS, level));
  return VOLUME_VALUES[level];
}

static int effectiveVolumeLevelForRec(int recNum) {
  int delta = (recNum > 0 && recNum <= MAX_REC) ? recVolumeDelta[recNum - 1] : 0;
  return max(0, min(VOL_LEVELS, globalVolumeLevel() + delta));
}

static void applyPlaybackVolumeForRec(int recNum) {
  if (speakerOutputReady) {
    speakerVolumeCurrent = volumeFromLevel(effectiveVolumeLevelForRec(recNum));
    M5Cardputer.Speaker.setVolume(speakerVolumeCurrent);
  }
}

static void applyBrightness() {
  M5Cardputer.Display.setBrightness(BRIGHT_VALUES[brightLevel]);
}

static void applyMinBrightness() {
  M5Cardputer.Display.setBrightness(BRIGHT_VALUES[0]);
}

static void adjustBrightness(int delta) {
  int level = (int)brightLevel;
  if (delta > 0) level++;
  else if (delta < 0) level--;
  brightLevel = (uint8_t)max(0, min((int)BRIGHT_LEVELS - 1, level));
  applyBrightness();
}

// 绛夋墍鏈夐敭鏉惧紑
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
  speakerColdStarted = false;
  speakerVolumeCurrent = 0;
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x13, 0x00, 400000);  // HP drive OFF
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x12, 0xFC, 400000);  // DAC 鎺夌數
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x32, 0x00, 400000);  // DAC鈫扝P 娣烽煶鍣ㄦ柇寮€
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
  M5Cardputer.In_I2C.writeRegister8(0x18, 0x14, 0x17, 400000);  // 寮€鏈洪娆″伓鍙戜綆澧炵泭, 閲嶅啓涓€娆＄‘淇濈敓鏁?
  micInputReady = ok;
  return ok;
}

static void processMicBuffer(int16_t *buf, size_t n, int32_t &dc, int32_t &hum, int32_t &lpf, int32_t &noiseRms, int32_t &softGateQ8, int gain = recGain) {
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
    int32_t x = (int32_t)(((int64_t)cleaned * gain * softGateQ8) >> 8);
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

// ---------- 閫氱敤 UI (妯睆 240x135) ----------
// 椤舵爮: 鏍囬(浜豢16) + 宸︿晶灏忕珫鏉＄偣缂€ + 涓嬪垎闅旂嚎
static void drawHeader(const char *title) {
  auto &d = M5Cardputer.Display;
  d.fillRect(2, 5, 3, 13, COL_GREEN);          // 鏍囬鍓嶇殑鑽у厜缁垮皬绔栨潯(鐐圭紑)
  FONT_CN_16(d);
  d.setTextColor(COL_GREEN, COL_BG);
  d.setCursor(10, 3);
  d.print(title);
  d.drawFastHLine(0, 21, d.width(), COL_DIM);
}

// 鐢甸噺: 鍐呭鍖哄彸涓婅灏忓瓧
static uint16_t dseg14Mask(char c) {
  if (c == 'o') return DS_C | DS_D | DS_E | DS_G;
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
  } else if (_c == '#') { \
    dsegLine((g), (x) + 2, (y) + 3, (x) + 2, (y) + 13, (col)); \
    dsegLine((g), (x) + 6, (y) + 3, (x) + 6, (y) + 13, (col)); \
    dsegLine((g), (x), (y) + 6, (x) + 8, (y) + 6, (col)); \
    dsegLine((g), (x), (y) + 10, (x) + 8, (y) + 10, (col)); \
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
  if (c == '#') return 11;
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

static void dsegLineScaled(m5gfx::M5GFX &g, int x1, int y1, int x2, int y2, uint16_t col, int scale) {
  if (scale < 1) scale = 1;
  x1 *= scale; y1 *= scale; x2 *= scale; y2 *= scale;
  for (int ox = 0; ox < scale; ox++) {
    for (int oy = 0; oy < scale; oy++) {
      g.drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, col);
    }
  }
}

static int drawDseg14CharScaled(m5gfx::M5GFX &g, int x, int y, char c, uint16_t col, int scale) {
  if (scale < 1) scale = 1;
  auto seg = [&](int x1, int y1, int x2, int y2) {
    dsegLineScaled(g, x / scale + x1, y / scale + y1, x / scale + x2, y / scale + y2, col, scale);
  };
  if (c == ':') {
    g.fillRect(x + 2 * scale, y + 4 * scale, 2 * scale, 2 * scale, col);
    g.fillRect(x + 2 * scale, y + 10 * scale, 2 * scale, 2 * scale, col);
  } else if (c == '#') {
    seg(2, 3, 2, 13);
    seg(6, 3, 6, 13);
    seg(0, 6, 8, 6);
    seg(0, 10, 8, 10);
  } else if (c == '_') {
    seg(1, 15, 7, 15);
  } else if (c == '+') {
    seg(1, 8, 7, 8);
    seg(4, 4, 4, 12);
  } else if (c == '%' || c == '/') {
    g.fillRect(x + 1 * scale, y + 2 * scale, 2 * scale, 2 * scale, col);
    g.fillRect(x + 6 * scale, y + 12 * scale, 2 * scale, 2 * scale, col);
    seg(7, 1, 1, 14);
  } else {
    uint16_t m = dseg14Mask(c);
    if (m & DS_A) seg(2, 0, 7, 0);
    if (m & DS_B) seg(8, 1, 8, 6);
    if (m & DS_C) seg(8, 9, 8, 14);
    if (m & DS_D) seg(2, 15, 7, 15);
    if (m & DS_E) seg(0, 9, 0, 14);
    if (m & DS_F) seg(0, 1, 0, 6);
    if (m & DS_G) seg(2, 8, 7, 8);
    if (m & DS_H) seg(1, 1, 3, 7);
    if (m & DS_I) seg(7, 1, 5, 7);
    if (m & DS_J) seg(3, 9, 1, 14);
    if (m & DS_K) seg(5, 9, 7, 14);
    if (m & DS_M) seg(4, 1, 4, 7);
    if (m & DS_N) seg(4, 9, 4, 14);
  }
  return dseg14CharWidth(c) * scale;
}

static int dseg14TextWidthScaled(const char *text, int scale) {
  return dseg14TextWidth(text) * ((scale < 1) ? 1 : scale);
}

static void drawDseg14TextScaled(m5gfx::M5GFX &g, int x, int y, const char *text, uint16_t col, int scale) {
  while (*text) x += drawDseg14CharScaled(g, x, y, *text++, col, scale);
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
  drawStatusBatteryColor(g, col);
  g.setTextColor(col, COL_BG);
  drawDseg14Text(g, 4, 1, text, col);
}

static void drawStatusTitleNoLine(M5Canvas &g, const char *text, uint16_t col = COL_GREEN) {
  g.fillRect(0, 0, CONTENT_W, 19, COL_BG);
  drawStatusBatteryColor(g, col);
  g.setTextColor(col, COL_BG);
  drawDseg14Text(g, 4, 1, text, col);
}

#define DRAW_STATUS_TAB_BODY(g, label, x, active) do { \
  uint16_t _tabCol = active ? recAccentColor() : recDimColor(); \
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
  (g).fillRect(0, 0, CONTENT_W, 21, COL_BG); \
  drawStatusBatteryColor(g, recAccentColor()); \
  drawStatusTab(g, "NOR", 4, (mode) == REC_NORMAL); \
  drawStatusTab(g, "QCK", 48, (mode) == REC_SHORTCUT); \
  drawStatusTab(g, "IMP", 92, (mode) == REC_IMPORTANT); \
  drawStatusTab(g, "MU", 136, (mode) == REC_MUSIC); \
} while (0)

static void drawStatusTabs(m5gfx::M5GFX &g, uint8_t mode, int count) {
  DRAW_STATUS_TABS_BODY(g, mode, count);
}

static void drawStatusTabs(M5Canvas &g, uint8_t mode, int count) {
  DRAW_STATUS_TABS_BODY(g, mode, count);
}

// 搴曟爮鎻愮ず(灏忓瓧, 涓婃柟涓€鏉″垎闅旂嚎)
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

static void drawRecColorInputBar(const char *hex, uint8_t len, bool bad = false) {
  auto &d = M5Cardputer.Display;
  d.fillRect(0, 113, CONTENT_W, 22, COL_BG);
  uint16_t col = bad ? COL_RED : recAccentColor();
  d.drawRect(0, 113, CONTENT_W, 22, col);
  FONT_ASCII(d);
  d.setTextColor(col, COL_BG);
  d.setCursor(8, 120);
  d.print("COLOR #");
  for (uint8_t i = 0; i < 6; i++) d.print(i < len ? hex[i] : '_');
}

static bool inputRecAccentPreset() {
  char hex[7] = {0};
  uint8_t len = 0;
  waitRelease();
  drawRecColorInputBar(hex, len);
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (keyEsc()) {
        drawActionToast("CANCEL", recDimColor());
        waitRelease();
        return false;
      }
      if (keyDel()) {
        if (len > 0) hex[--len] = 0;
        drawRecColorInputBar(hex, len);
        waitRelease();
        continue;
      }
      if (keyEnter()) {
        uint32_t rgb = 0;
        if (len == 6 && parseHexRgb(hex, rgb)) {
          uint16_t col = rgb888To565(rgb);
          setRecCustomAccent(col);
          bool saved = false;
          if (sdMount()) {
            saved = saveRecAccentStateMounted();
            SD.end();
          }
          drawActionToast(saved ? "SAVED" : "NO SD", saved ? recAccentColor() : COL_RED);
          waitRelease();
          return true;
        }
        drawRecColorInputBar(hex, len, true);
        waitRelease();
        continue;
      }
      char k = pressedBindKey();
      int hv = hexValue(k);
      if (hv >= 0 && len < 6) {
        hex[len++] = (char)(hv < 10 ? ('0' + hv) : ('A' + hv - 10));
        hex[len] = 0;
        drawRecColorInputBar(hex, len);
      }
      waitRelease();
    }
    delay(8);
  }
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
  int level = globalVolumeLevel();
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
  int level = globalVolumeLevel();
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
  drawDseg14Text(cv, x, y, "SAVED", recAccentColor());
  cv.fillRect(x, y + 17, 54, 3, recAccentColor());
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
  drawFooter("鎸変换鎰忛敭杩斿洖");
  waitRelease();
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
    delay(8);
  }
}

// ---------- 蹇嵎閿粦瀹? 璇?鍐?SD ----------
static void saveHotkeys();

static void readHotkeyFile(File &f, uint8_t group) {
  while (f.available() && hotkeyCount[group] < MAX_HOTKEY) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;
    char k = normalizeBindKey(line.charAt(0));
    int idx = line.substring(2).toInt();
    if (k && idx > 0) {
      hotkeys[group][hotkeyCount[group]].key = k;
      hotkeys[group][hotkeyCount[group]].idx = idx;
      hotkeyCount[group]++;
    }
  }
}

static void loadHotkeys() {
  memset(hotkeyCount, 0, sizeof(hotkeyCount));
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
    while (f.available() && hotkeyCount[0] < MAX_HOTKEY) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 3) continue;        // 鏍煎紡: "<閿? <缂栧彿>"
      char k = normalizeBindKey(line.charAt(0));
      int idx = line.substring(2).toInt();
      if (k && idx > 0) { hotkeys[0][hotkeyCount[0]].key = k; hotkeys[0][hotkeyCount[0]].idx = idx; hotkeyCount[0]++; }
    }
    f.close();
  }
  for (uint8_t group = 0; group < HOTKEY_GROUPS; group++) {
    File gf = SD.open(HOTKEY_GROUP_PATHS[group], FILE_READ);
    if (gf) {
      hotkeyCount[group] = 0;
      readHotkeyFile(gf, group);
      gf.close();
    }
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
  for (uint8_t group = 0; group < HOTKEY_GROUPS; group++) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "/SHORTCUT/keys_%d.tmp", (int)group + 1);
    SD.remove(tmp);
    File f = SD.open(tmp, FILE_WRITE);
    if (f) {
      for (int i = 0; i < hotkeyCount[group]; i++) f.printf("%c %d\n", hotkeys[group][i].key, hotkeys[group][i].idx);
      f.flush();
      f.close();
      SD.remove(HOTKEY_GROUP_PATHS[group]);
      if (!SD.rename(tmp, HOTKEY_GROUP_PATHS[group])) SD.remove(tmp);
    }
  }
  SD.remove(HOTKEY_PATH);
  File legacy = SD.open(HOTKEY_PATH, FILE_WRITE);
  if (legacy) {
    for (int i = 0; i < hotkeyCount[0]; i++) legacy.printf("%c %d\n", hotkeys[0][i].key, hotkeys[0][i].idx);
    legacy.close();
  }
  SD.end();
}

static int findHotkey(char k) {
  k = normalizeBindKey(k);
  if (!k) return -1;
  for (int i = 0; i < hotkeyCount[hotkeyGroup]; i++) if (hotkeys[hotkeyGroup][i].key == k) return hotkeys[hotkeyGroup][i].idx;
  return -1;
}

// Keep sample-key preview aligned with the shortcut list selection.
static void selectHotkeyPreviewRec(int recNum) {
  if (recNum <= 0) return;
  g_listMode = REC_SHORTCUT;
  g_listModeSelectedRec[REC_SHORTCUT] = recNum;
}

// Bind one recording to one shortcut key.
static void setHotkey(char k, int idx) {
  k = normalizeBindKey(k);
  if (!k || idx <= 0) return;
  for (int i = 0; i < hotkeyCount[hotkeyGroup]; i++) {
    if (hotkeys[hotkeyGroup][i].key == k || hotkeys[hotkeyGroup][i].idx == idx) {
      for (int j = i; j < hotkeyCount[hotkeyGroup] - 1; j++) hotkeys[hotkeyGroup][j] = hotkeys[hotkeyGroup][j + 1];
      hotkeyCount[hotkeyGroup]--; i--;
    }
  }
  if (hotkeyCount[hotkeyGroup] < MAX_HOTKEY) {
    hotkeys[hotkeyGroup][hotkeyCount[hotkeyGroup]].key = k;
    hotkeys[hotkeyGroup][hotkeyCount[hotkeyGroup]].idx = idx;
    hotkeyCount[hotkeyGroup]++;
  }
  saveHotkeys();
}

static char hotkeyOf(int idx) {  // 璇ュ綍闊崇紪鍙锋槸鍚﹀凡缁戝畾鏌愰敭, 杩斿洖閿?0=鏃?
  for (int i = 0; i < hotkeyCount[hotkeyGroup]; i++) if (hotkeys[hotkeyGroup][i].idx == idx) return hotkeys[hotkeyGroup][i].key;
  return 0;
}

// 褰撳墠鏄惁鎸変笅浜嗕竴涓?宸茬粦瀹氱殑鎾斁閿?(闈?Ctrl/Tab): 杩斿洖瀵瑰簲褰曢煶缂栧彿, 鍚﹀垯 -1
static int pressedHotkeyRec() {
  if (keyCtrl() || keyTab() || keyFn() || keyAlt()) return -1;
  if (keyEnter() || keyDel() || keySpace() || keyUpload()) return -1;
  char bk = pressedBindKey();
  if (!bk) return -1;
  return findHotkey(bk);             // >0 宸茬粦瀹? -1 鏈粦瀹?
}

// ---------- 鍒楀嚭褰曢煶缂栧彿 ----------
int recList[MAX_REC];
int recCount = 0;
static uint8_t shortcutBits[(MAX_REC + 8) / 8];
static uint8_t importantBits[(MAX_REC + 8) / 8];
static uint8_t musicBits[(MAX_REC + 8) / 8];
static uint8_t frictionDoneBits[(MAX_REC + 8) / 8];
static uint8_t frictionPendingBits[(MAX_REC + 8) / 8];
static uint8_t uploadQueuedBits[(MAX_REC + 8) / 8];
static uint8_t uploadDoneBits[(MAX_REC + 8) / 8];
static uint8_t uploadPendingBits[(MAX_REC + 8) / 8];
static uint8_t uploadModelErrBits[(MAX_REC + 8) / 8];
static uint8_t uploadJobErrBits[(MAX_REC + 8) / 8];
static uint8_t shortcutTrimTier[MAX_REC];
static uint16_t recDurationSec[MAX_REC];

static void clearImportantBits() {
  memset(shortcutBits, 0, sizeof(shortcutBits));
  memset(importantBits, 0, sizeof(importantBits));
  memset(musicBits, 0, sizeof(musicBits));
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

static uint16_t durationFromPcm(uint32_t dataBytes, uint32_t sampleRate, uint16_t channels) {
  if (dataBytes == 0 || sampleRate == 0 || channels == 0) return 0;
  uint32_t frameBytes = (uint32_t)channels * 2;
  uint32_t frames = dataBytes / frameBytes;
  uint32_t sec = (frames + sampleRate - 1) / sampleRate;
  return (sec > 65535) ? 65535 : (uint16_t)sec;
}

static uint16_t durationFromFileSize(uint32_t fileSize) {
  if (fileSize <= 44) return 0;
  return durationFromPcm(fileSize - 44, REC_RATE, 1);
}

static uint16_t rd16le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool parseWavInfo(File &f, WavInfo &info) {
  info = WavInfo();
  uint32_t total = f.size();
  if (total < 44) { info.error = "WAV SHORT"; return false; }
  uint8_t h[12];
  if (!f.seek(0) || f.read(h, sizeof(h)) != (int)sizeof(h)) { info.error = "WAV READ"; return false; }
  if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) { info.error = "NOT RIFF"; return false; }

  bool haveFmt = false;
  bool haveData = false;
  uint16_t audioFormat = 0;
  uint32_t pos = 12;
  while (pos + 8 <= total) {
    uint8_t ch[8];
    if (!f.seek(pos) || f.read(ch, sizeof(ch)) != (int)sizeof(ch)) { info.error = "WAV CHUNK"; return false; }
    uint32_t chunkSize = rd32le(ch + 4);
    uint32_t payload = pos + 8;
    if (payload > total || chunkSize > total - payload) { info.error = "WAV SIZE"; return false; }

    if (memcmp(ch, "fmt ", 4) == 0) {
      if (chunkSize < 16) { info.error = "FMT SHORT"; return false; }
      uint8_t fmt[16];
      if (!f.seek(payload) || f.read(fmt, sizeof(fmt)) != (int)sizeof(fmt)) { info.error = "FMT READ"; return false; }
      audioFormat = rd16le(fmt);
      info.channels = rd16le(fmt + 2);
      info.sampleRate = rd32le(fmt + 4);
      info.blockAlign = rd16le(fmt + 12);
      info.bitsPerSample = rd16le(fmt + 14);
      haveFmt = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      info.dataOffset = payload;
      info.dataBytes = chunkSize;
      haveData = true;
    }

    uint32_t next = payload + chunkSize + (chunkSize & 1);
    if (next <= pos) { info.error = "WAV LOOP"; return false; }
    pos = next;
  }

  if (!haveFmt) { info.error = "NO FMT"; return false; }
  if (!haveData || info.dataBytes == 0) { info.error = "NO DATA"; return false; }
  if (audioFormat != 1) { info.error = "PCM ONLY"; return false; }
  if (info.bitsPerSample != 16) { info.error = "16BIT ONLY"; return false; }
  if (info.channels != 1 && info.channels != 2) { info.error = "CH 1/2 ONLY"; return false; }
  if (info.sampleRate < 8000 || info.sampleRate > 48000) { info.error = "RATE 8-48K"; return false; }
  uint16_t expectedAlign = info.channels * (info.bitsPerSample / 8);
  if (info.blockAlign != expectedAlign) { info.error = "BAD ALIGN"; return false; }
  info.dataBytes -= info.dataBytes % info.blockAlign;
  if (info.dataBytes == 0) { info.error = "NO DATA"; return false; }
  info.error = nullptr;
  return true;
}

static uint32_t playableWavDataBytes(File &f) {
  WavInfo info;
  return parseWavInfo(f, info) ? info.dataBytes : 0;
}

static uint32_t wavSampleRate(File &f) {
  WavInfo info;
  return parseWavInfo(f, info) ? info.sampleRate : REC_RATE;
}

static uint16_t wavDurationSec(File &f) {
  WavInfo info;
  if (!parseWavInfo(f, info)) return 0;
  return durationFromPcm(info.dataBytes, info.sampleRate, info.channels);
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

static bool isMusicRec(int recNum) {
  if (recNum <= 0 || recNum > MAX_REC) return false;
  int bit = recNum - 1;
  return (musicBits[bit >> 3] & (1 << (bit & 7))) != 0;
}

static void setShortcutRec(int recNum, bool shortcut) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (shortcut) shortcutBits[bit >> 3] |= (1 << (bit & 7));
  else {
    shortcutBits[bit >> 3] &= ~(1 << (bit & 7));
    shortcutTrimTier[recNum - 1] = 0;
  }
}

static void setImportantRec(int recNum, bool important) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (important) importantBits[bit >> 3] |= (1 << (bit & 7));
  else importantBits[bit >> 3] &= ~(1 << (bit & 7));
}

static void setMusicRec(int recNum, bool music) {
  if (recNum <= 0 || recNum > MAX_REC) return;
  int bit = recNum - 1;
  if (music) musicBits[bit >> 3] |= (1 << (bit & 7));
  else musicBits[bit >> 3] &= ~(1 << (bit & 7));
}

static uint8_t recKindOf(int recNum) {
  if (isShortcutRec(recNum)) return REC_SHORTCUT;
  if (isImportantRec(recNum)) return REC_IMPORTANT;
  if (isMusicRec(recNum)) return REC_MUSIC;
  return REC_NORMAL;
}

static void recordingPathForRec(int recNum, char *p, size_t n) {
  recordingPathKind(recNum, recKindOf(recNum), p, n);
}

static float targetHzForC4Semis(int8_t semis) {
  return 261.625565f * powf(2.0f, (float)semis / 12.0f);
}

static float detectPitchHzInOpenWav(File &f, const WavInfo &info) {
  uint32_t rate = info.sampleRate;
  uint32_t dataBytes = info.dataBytes;
  if (rate < 8000 || rate > 48000 || dataBytes < 512) return 0.0f;
  static int16_t pitchBuf[1024];
  const uint16_t frameN = 1024;
  const uint32_t maxBytes = min(dataBytes, rate * (uint32_t)info.blockAlign * 2UL);
  const int minLag = max(8, (int)(rate / 1200));
  const int maxLag = min((int)frameN - 4, (int)(rate / 75));
  float bestHz = 0.0f;
  float bestScore = 0.0f;
  uint32_t offset = 0;

  while (offset + frameN * 2 <= maxBytes) {
    if (!f.seek(info.dataOffset + offset)) break;
    int rd = f.read((uint8_t *)pitchBuf, frameN * 2);
    if (rd != (int)(frameN * 2)) break;

    int64_t sum = 0;
    for (uint16_t i = 0; i < frameN; i++) sum += pitchBuf[i];
    int32_t mean = (int32_t)(sum / frameN);
    int64_t energy = 0;
    for (uint16_t i = 0; i < frameN; i++) {
      int32_t s = (int32_t)pitchBuf[i] - mean;
      energy += (int64_t)s * s;
    }
    float rms = sqrtf((float)((double)energy / frameN));
    if (rms > 450.0f && energy > 0) {
      int bestLag = 0;
      int64_t bestCorr = 0;
      for (int lag = minLag; lag <= maxLag; lag++) {
        int64_t corr = 0;
        int64_t lagEnergy = 0;
        for (uint16_t i = 0; i + lag < frameN; i++) {
          int32_t a = (int32_t)pitchBuf[i] - mean;
          int32_t b = (int32_t)pitchBuf[i + lag] - mean;
          corr += (int64_t)a * b;
          lagEnergy += (int64_t)a * a;
        }
        if (corr > bestCorr && lagEnergy > 0) {
          bestCorr = corr;
          bestLag = lag;
        }
      }
      if (bestLag > 0) {
        float score = (float)((double)bestCorr / (double)energy);
        if (score > bestScore) {
          bestScore = score;
          bestHz = (float)rate / (float)bestLag;
        }
      }
    }
    offset += frameN * 2;
  }

  return bestScore >= 0.30f ? bestHz : 0.0f;
}

static float scalePitchHzForRec(int recNum) {
  if (recNum <= 0) return 0.0f;
  if (g_scalePitchCacheRec == recNum && g_scalePitchCacheHz > 0.0f) return g_scalePitchCacheHz;
  if (!sdMount()) return 0.0f;
  char p[128];
  recordingPathForRec(recNum, p, sizeof(p));
  File f = SD.open(p, FILE_READ);
  float hz = 0.0f;
  if (f) {
    WavInfo info;
    if (parseWavInfo(f, info)) hz = detectPitchHzInOpenWav(f, info);
    f.close();
  }
  SD.end();
  if (hz > 0.0f) {
    g_scalePitchCacheRec = recNum;
    g_scalePitchCacheHz = hz;
  }
  return hz;
}

static bool prepareScaleNoteForRec(int recNum, int8_t targetSemis) {
  if (recNum <= 0) return false;
  float sourceHz = scalePitchHzForRec(recNum);
  if (sourceHz <= 0.0f) sourceHz = 261.625565f;
  float targetHz = targetHzForC4Semis(targetSemis);
  float semis = 12.0f * (logf(targetHz / sourceHz) / logf(2.0f));
  g_shortcutSemisOverride = semis + (float)g_shortcutTransposeSemis;
  return true;
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
    if (kind == REC_MUSIC && recKindOf(recNum) == REC_MUSIC) setMusicRec(recNum, true);
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
  setMusicRec(recNum, kind == REC_MUSIC);
  recCount++;
}

static void insertRecListAtEnd(int recNum, uint8_t kind = REC_NORMAL) {
  if (recNum <= 0) return;
  int existing = recListIndexOf(recNum);
  if (existing >= 0) {
    if (kind == REC_SHORTCUT) setShortcutRec(recNum, true);
    if (kind == REC_IMPORTANT) setImportantRec(recNum, true);
    if (kind == REC_MUSIC && recKindOf(recNum) == REC_MUSIC) setMusicRec(recNum, true);
    return;
  }
  if (recCount >= MAX_REC) return;
  recList[recCount++] = recNum;
  setShortcutRec(recNum, kind == REC_SHORTCUT);
  setImportantRec(recNum, kind == REC_IMPORTANT);
  setMusicRec(recNum, kind == REC_MUSIC);
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
  char p[128];
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
      if (kind == REC_MUSIC) {
        if (!e.isDirectory() && isWavFileName(e.name()) && musicTrackCount < MAX_MUSIC_TRACKS) {
          uint16_t dur = wavDurationSec(e);
          if (dur == 0) { e.close(); continue; }
          int recId = musicRecIdForSlot(musicTrackCount);
          if (recId > 0) {
            const char *name = e.name();
            if (name && name[0] == '/') snprintf(musicPaths[musicTrackCount], sizeof(musicPaths[musicTrackCount]), "%s", name);
            else snprintf(musicPaths[musicTrackCount], sizeof(musicPaths[musicTrackCount]), "%s/%s", dirPath, name ? name : "");
            insertRecListAtEnd(recId, REC_MUSIC);
            recDurationSec[recId - 1] = dur;
            musicTrackCount++;
          }
        }
      } else {
        int n = parseRecordingNumber(e.name());
        if (n > 0) {
          uint16_t dur = (!e.isDirectory()) ? wavDurationSec(e) : 0;
          if (dur == 0) { e.close(); continue; }
          if (n > maxIdx) maxIdx = n;
          insertRecListSorted(n, kind);
          recDurationSec[n - 1] = dur;
        }
      }
      e.close();
    }
    dir.close();
  }
}

static bool loadNormalRecordingMounted(int recNum) {
  if (recNum <= 0 || recNum >= MUSIC_ID_BASE) return false;
  if (recListIndexOf(recNum) >= 0) return false;
  char p[128];
  recordingPathKind(recNum, REC_NORMAL, p, sizeof(p));
  File f = SD.open(p, FILE_READ);
  if (!f) return false;
  uint16_t dur = wavDurationSec(f);
  f.close();
  if (dur == 0) return false;
  insertRecListSorted(recNum, REC_NORMAL);
  recDurationSec[recNum - 1] = dur;
  if (normalOldestLoaded == 0 || recNum < normalOldestLoaded) normalOldestLoaded = recNum;
  return true;
}

static int recentNormalScanStartMounted() {
  int next = readNextIndexCache();
  if (next > 1 && next <= MUSIC_ID_BASE) return next - 1;
  if (next > MUSIC_ID_BASE) return MUSIC_ID_BASE - 1;
  return MUSIC_ID_BASE - 1;
}

static void scanRecentNormalRecordingsMounted(uint8_t limit) {
  normalOldestLoaded = 0;
  normalReachedBeginning = false;
  int found = 0;
  for (int idx = recentNormalScanStartMounted(); idx >= 1 && found < limit; idx--) {
    if (loadNormalRecordingMounted(idx)) found++;
    if (idx == 1) normalReachedBeginning = true;
  }
  if (found == 0) normalReachedBeginning = true;
}

static bool loadOlderNormalRecordings(uint8_t limit = NORMAL_OLDER_LOAD) {
  if (normalReachedBeginning) return false;
  if (!sdMount()) return false;
  int start = (normalOldestLoaded > 1) ? normalOldestLoaded - 1 : recentNormalScanStartMounted();
  int found = 0;
  for (int idx = start; idx >= 1 && found < limit; idx--) {
    if (loadNormalRecordingMounted(idx)) found++;
    if (idx == 1) normalReachedBeginning = true;
  }
  SD.end();
  return found > 0;
}

static int lowestAvailableRecordingIndex() {
  char p[128];
  for (int idx = 1; idx < MUSIC_ID_BASE; idx++) {
    recordingPathKind(idx, REC_NORMAL, p, sizeof(p));
    if (!SD.exists(p) && !recordingExistsKind(idx, REC_SHORTCUT) && !recordingExistsKind(idx, REC_IMPORTANT) && !recordingExistsKind(idx, REC_MUSIC)) return idx;
  }
  return MUSIC_ID_BASE - 1;
}

static int nextRecordingIndex() {
  int idx = nextRecHint;
  if (idx < 1 || idx > 9999) idx = readNextIndexCache();
  if (idx < 1 || idx > 9999) idx = 1;
  if (idx >= MUSIC_ID_BASE) idx = 1;
  int start = idx;
  do {
    if (!recordingExistsKind(idx, REC_NORMAL) &&
        !recordingExistsKind(idx, REC_SHORTCUT) &&
        !recordingExistsKind(idx, REC_IMPORTANT) &&
        !recordingExistsKind(idx, REC_MUSIC)) {
      nextRecHint = idx;
      return idx;
    }
    idx = (idx < MUSIC_ID_BASE - 1) ? idx + 1 : 1;
  } while (idx != start);
  idx = lowestAvailableRecordingIndex();
  nextRecHint = idx;
  writeNextIndexCache(idx);
  return idx;
}

static void scanRecordings(bool compactNext = false) {
  recCount = 0;
  clearImportantBits();
  resetMusicTracks();
  if (!sdMount()) return;
  loadRecAccentPresetMounted();
  if (!SD.exists(REC_DIR) && !SD.exists(SHORTCUT_DIR) && !SD.exists(IMPORTANT_DIR) && !SD.exists(MUSIC_DIR) && !SD.exists("/music")) { SD.end(); return; }
  int maxIdx = 0;
  if (SD.exists(REC_DIR)) scanRecentNormalRecordingsMounted(NORMAL_INITIAL_LOAD);
  scanRecordingDir(SHORTCUT_DIR, REC_SHORTCUT, maxIdx);
  scanRecordingDir(IMPORTANT_DIR, REC_IMPORTANT, maxIdx);
  if (SD.exists(MUSIC_DIR)) scanRecordingDir(MUSIC_DIR, REC_MUSIC, maxIdx);
  else if (SD.exists("/music")) scanRecordingDir("/music", REC_MUSIC, maxIdx);
  applyRecordingOrderMounted();
  loadUploadStateMounted();
  loadRecVolumeStateMounted();
  nextRecHint = readNextIndexCache();
  if (nextRecHint < 1 || nextRecHint >= MUSIC_ID_BASE) {
    nextRecHint = lowestAvailableRecordingIndex();
    writeNextIndexCache(nextRecHint);
  }
  SD.end();
}

// ---------- 鎻愮ず闊?"婊? (娣″叆娣″嚭) ----------
// (淇濈暀: 鏆傛湭浣跨敤; 闇€瑕佹椂鍙湪鍒囧埌鎵０鍣ㄥ悗璋冪敤浠ラ伄鐖嗛煶)

// ---------- 鍒囨崲缂栬В鐮佸櫒鍒版壃澹板櫒(鍚墜鍔ㄥ紑 DAC) ----------
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

static bool trimShortcutSilenceForRec(int recNum, uint8_t tier = 1) {
  if (recNum <= 0) return false;
  if (!sdMount()) return false;
  char p[40];
  recordingPathKind(recNum, REC_SHORTCUT, p, sizeof(p));
  if (tier < 1) tier = 1;
  if (tier > 3) tier = 3;
  int32_t threshold = SHORTCUT_TRIM_RMS;
  uint32_t headPad = SHORTCUT_HEAD_PAD;
  uint32_t tailPad = SHORTCUT_TAIL_PAD;
  if (tier == 2) {
    threshold = SHORTCUT_RETRIM_RMS;
    headPad = SHORTCUT_RETRIM_HEAD_PAD;
    tailPad = SHORTCUT_RETRIM_TAIL_PAD;
  } else if (tier >= 3) {
    threshold = SHORTCUT_SUPERTRIM_RMS;
    headPad = SHORTCUT_SUPERTRIM_HEAD_PAD;
    tailPad = SHORTCUT_SUPERTRIM_TAIL_PAD;
  }
  bool ok = trimSilenceInWav(p, threshold, headPad, tailPad);
  if (ok) {
    File f = SD.open(p, FILE_READ);
    if (f) { setRecDuration(recNum, f.size()); f.close(); }
  }
  SD.end();
  return ok;
}

static uint8_t nextShortcutTrimTier(int recNum, bool alreadyShortcut) {
  if (recNum <= 0 || recNum > MAX_REC) return 1;
  uint8_t &tier = shortcutTrimTier[recNum - 1];
  if (!alreadyShortcut) tier = 1;
  else if (tier < 2) tier = 2;
  else if (tier < 3) tier++;
  else tier = 3;
  return tier;
}

static bool markRecordingShortcut(int recNum, uint8_t *trimTierOut = nullptr) {
  bool alreadyShortcut = isShortcutRec(recNum);
  bool ok = moveRecordingToKind(recNum, REC_SHORTCUT);
  uint8_t tier = nextShortcutTrimTier(recNum, alreadyShortcut);
  if (ok) trimShortcutSilenceForRec(recNum, tier);
  if (trimTierOut) *trimTierOut = tier;
  return ok;
}

static void speakerSetVolume(int volume) {
  speakerVolumeCurrent = max(0, min(255, volume));
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(speakerVolumeCurrent);
}

static void speakerCancelIdleOff() {
  speakerIdleOffAtMs = 0;
}

static void speakerFadeToVolume(int target, uint16_t durationMs = 24) {
  if (!speakerOutputReady) return;
  target = max(0, min(255, target));
  int start = speakerVolumeCurrent;
  if (start == target || durationMs == 0) {
    speakerSetVolume(target);
    return;
  }
  const uint8_t steps = 8;
  uint16_t stepDelay = max((uint16_t)1, (uint16_t)(durationMs / steps));
  for (uint8_t i = 1; i <= steps; i++) {
    int v = start + (int)((long)(target - start) * i / steps);
    speakerSetVolume(v);
    delay(stepDelay);
  }
}

static int playbackVolumeForRec(int recNum) {
  return volumeFromLevel(effectiveVolumeLevelForRec(recNum));
}

static int playbackVolumeForRecMode(int recNum, bool isHotkeyPlayback) {
  int level = effectiveVolumeLevelForRec(recNum);
  (void)isHotkeyPlayback;
  return volumeFromLevel(level);
}

static void fadeInPlaybackVolumeForRec(int recNum) {
  speakerSetVolume(playbackVolumeForRec(recNum));
}

static void speakerSoftStop(uint16_t fadeMs = 24) {
  if (!speakerOutputReady) {
    M5Cardputer.Speaker.stop();
    return;
  }
  speakerCancelIdleOff();
  speakerFadeToVolume(0, fadeMs);
  M5Cardputer.Speaker.stop();
}

static void speakerKeepAliveSilence() {
  if (!speakerOutputReady) return;
  if (M5Cardputer.Speaker.isPlaying(0) >= 2) return;
  M5Cardputer.Speaker.playRaw(speakerSilenceBuf, PB_N, REC_RATE, false, 1, 0, false);
}

static void speakerPrimeAfterColdStart() {
  if (!speakerOutputReady || !speakerColdStarted) return;
  speakerApplyOutputRoute();
  M5Cardputer.Speaker.playRaw(speakerSilenceBuf, PB_N, REC_RATE, false, 1, 0, false);
  delay(18);
  speakerApplyOutputRoute();
  speakerColdStarted = false;
}

static void speakerQuietFlush(uint16_t fadeMs = 6) {
  if (!speakerOutputReady) return;
  speakerCancelIdleOff();
  speakerFadeToVolume(0, fadeMs);
  M5Cardputer.Speaker.stop();
}

static void speakerScheduleIdleOff(uint32_t delayMs = SPEAKER_IDLE_OFF_MS) {
  if (!speakerOutputReady) return;
  speakerSetVolume(0);
  speakerIdleOffAtMs = millis() + delayMs;
  speakerKeepAliveSilence();
}

static void speakerApplyOutputRoute() {
  const uint8_t ES = 0x18;
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x01, 0xB5, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x02, 0x18, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x12, 0x00, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x10, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0xBF, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x37, 0x08, 400000);
}

static void speakerIdlePowerDown() {
  if (!speakerOutputReady) return;
  speakerCancelIdleOff();
  speakerSetVolume(0);
  const uint8_t ES = 0x18;
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0x00, 400000);
  delay(2);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x00, 400000);
  delay(5);
  M5Cardputer.Speaker.stop();
  M5Cardputer.Speaker.end();
  speakerOutputReady = false;
  speakerColdStarted = false;
  speakerVolumeCurrent = 0;
  speakerDrainBeforeNextPlayback = false;
}

static void speakerMaybeIdleOff() {
  if (!speakerOutputReady || speakerIdleOffAtMs == 0) return;
  if ((int32_t)(millis() - speakerIdleOffAtMs) >= 0) {
    speakerIdlePowerDown();
    return;
  }
  speakerKeepAliveSilence();
}

static void speakerOn(bool muted = false) {
  speakerCancelIdleOff();
  if (speakerOutputReady) {
    speakerApplyOutputRoute();
    speakerSetVolume(muted ? 0 : playVol);
    return;
  }
  micInputReady = false;
  M5Cardputer.Mic.end();       // 鍏抽害(閲婃斁鍏辩敤缂栬В鐮佸櫒)
  M5Cardputer.Speaker.begin();
  speakerVolumeCurrent = muted ? 0 : playVol;
  M5Cardputer.Speaker.setVolume(speakerVolumeCurrent);
  speakerApplyOutputRoute();
  const uint8_t ES = 0x18;     // internal_spk=false 鏃犺嚜鍔?DAC 鍥炶皟, 鎵嬪姩寮€(鐓у畼鏂规壃澹板櫒瀵勫瓨鍣?
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x01, 0xB5, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x02, 0x18, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x12, 0x00, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x10, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0xBF, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x37, 0x08, 400000);
  speakerOutputReady = true;
  speakerColdStarted = true;
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

static const int WAVE_BARS = 60;   // 60 脳 4px = 240px (CONTENT_W)
static int8_t waveBars[WAVE_BARS]; // A绾块噰鏍风偣: -A_HALF..+A_HALF
static uint8_t waveBarCounts[WAVE_BARS];
static int8_t recLiveWave[CONTENT_W];      // B绾胯瑙夌紦瀛? 褰曢煶鐩戝惉绾?
static int8_t playbackLiveWave[CONTENT_W]; // B绾胯瑙夌紦瀛? 鎾斁鐩戝惉绾?

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

static bool previewPlaybackWaveBar(File &f, uint32_t dataOffset, uint32_t dataSize, uint16_t blockAlign, uint8_t &previewBar) {
  if (previewBar >= WAVE_BARS || dataSize == 0) return false;
  static int16_t tmp[REC_N];
  uint32_t restore = f.position();
  uint32_t segStart = (uint32_t)((uint64_t)dataSize * previewBar / WAVE_BARS);
  uint32_t segEnd = (uint32_t)((uint64_t)dataSize * (previewBar + 1) / WAVE_BARS);
  if (blockAlign == 0) blockAlign = 2;
  if (segEnd <= segStart) segEnd = segStart + blockAlign;
  if (segEnd > dataSize) segEnd = dataSize;
  uint32_t span = segEnd - segStart;
  uint32_t pos = segStart + span / 2;
  if (pos + sizeof(tmp) > segEnd) pos = (segEnd > sizeof(tmp)) ? (segEnd - sizeof(tmp)) : segStart;
  pos -= pos % blockAlign;
  uint32_t want = dataSize - pos;
  if (want > sizeof(tmp)) want = sizeof(tmp);
  if (want > segEnd - pos) want = segEnd - pos;
  want -= want % blockAlign;
  int rd = 0;
  if (want > 0 && f.seek(dataOffset + pos)) rd = f.read((uint8_t *)tmp, want);
  if (rd > 0) feedPlaybackWaveBars(pos, dataSize, tmp, rd / 2);
  f.seek(restore);
  previewBar++;
  return true;
}

static void previewPlaybackWaveStep(File &f, uint32_t dataOffset, uint32_t dataSize, uint16_t blockAlign, uint8_t &previewBar, uint8_t budget) {
  while (budget-- > 0 && previewPlaybackWaveBar(f, dataOffset, dataSize, blockAlign, previewBar)) {}
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
  cv.print("DEL");
  cv.drawRect(168, 125, 70, 7, COL_RED);
  cv.fillRect(168, 125, deleteProgressW(heldMs), 7, COL_RED);
}

static void drawDeleteProgress(m5gfx::M5GFX &g, uint32_t heldMs) {
  if (heldMs == 0) return;
  g.fillRect(112, 119, 126, 15, COL_BG);
  FONT_CN_12(g);
  g.setTextColor(COL_RED, COL_BG);
  g.setCursor(112, 120);
  g.print("DEL");
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
  d.print("DEL");
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
  drawDseg14Text(cv, 4, 1, recTime, recAccentColor());
  drawDeleteProgressLocal(cv, deleteHeldMs);
}

static void drawPlaybackBottomCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, uint32_t deleteHeldMs = 0) {
  cv.fillScreen(COL_BG);
  uint32_t cur = (played / 2) / REC_RATE;
  uint32_t tot = (dataSize / 2) / REC_RATE;
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(cv, 4, 1, curBuf, recAccentColor());
  if (deleteHeldMs > 0) {
    drawDeleteProgressLocal(cv, deleteHeldMs);
  } else {
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(cv, CONTENT_W - dseg14TextWidth(totBuf) - 4, 1, totBuf, recDimColor());
  }
}

static void drawTrackBar(m5gfx::M5GFX &g, int i, int v) {
  int x0 = i * CONTENT_W / WAVE_BARS;
  int x1 = (i + 1) * CONTENT_W / WAVE_BARS;
  int x = (x0 + x1) / 2;
  v = abs(v);
  if (v < 1) v = 1;
  if (v > A_HALF) v = A_HALF;
  g.drawFastVLine(x, A_CY - v, v * 2 + 1, recAccentColor());
}

static void drawTrackBar(M5Canvas &cv, int i, int v) {
  int x0 = i * CONTENT_W / WAVE_BARS;
  int x1 = (i + 1) * CONTENT_W / WAVE_BARS;
  int x = (x0 + x1) / 2;
  v = abs(v);
  if (v < 1) v = 1;
  if (v > A_HALF) v = A_HALF;
  cv.drawFastVLine(x, A_CY - v, v * 2 + 1, recAccentColor());
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
  cv.drawFastVLine(x, A_CY - WAVE_TOP - v, v * 2 + 1, recAccentColor());
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
    cv.drawLine(px, py, x, y, recAccentColor());
    px = x;
    py = y;
  }
}

static void drawLiveWaveVisual(m5gfx::M5GFX &g, const int8_t *src, int cy) {
  if (!src) return;
  int px = 0, py = cy - src[0];
  for (int x = 1; x < CONTENT_W; x++) {
    int y = cy - src[x];
    g.drawLine(px, py, x, y, recAccentColor());
    px = x;
    py = y;
  }
}

// 鎾斁鐢婚潰: A绾?杞ㄩ亾绾?Timeline A, B绾?鐩戝惉绾?Monitor B(鎾斁缂撳啿)
static void drawPlaybackCanvas(M5Canvas &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  (void)paused;
  updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(g, playbackLiveWave, B_CY);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  g.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  g.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  g.setTextColor(recAccentColor(), COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, recAccentColor());
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    g.setTextColor(recDimColor(), COL_BG);
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, recDimColor());
  }
}

static void drawPlaybackCanvas(m5gfx::M5GFX &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN, bool paused = false, uint32_t deleteHeldMs = 0) {
  (void)paused;
  updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(g, playbackLiveWave, B_CY);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  g.drawFastVLine(headX, WAVE_TOP, WAVE_CY - WAVE_TOP, COL_WHITE);

  uint32_t cur  = (played / 2) / REC_RATE;
  uint32_t tot  = (dataSize / 2) / REC_RATE;
  g.fillRect(0, WAVE_BOT + 1, CONTENT_W, 135 - WAVE_BOT - 1, COL_BG);
  g.setTextColor(recAccentColor(), COL_BG);
  char curBuf[8];
  snprintf(curBuf, sizeof(curBuf), "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, recAccentColor());
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    g.setTextColor(recDimColor(), COL_BG);
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, recDimColor());
  }
}

static void drawPlaybackWaveCanvas(M5Canvas &cv, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN) {
  if (liveWave && liveN > 0) updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  cv.fillScreen(COL_BG);
  cv.drawFastHLine(0, A_CY - WAVE_TOP, CONTENT_W, recAxisColor());
  drawTrackBarsLocal(cv);
  cv.drawFastHLine(0, B_CY - WAVE_TOP, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(cv, playbackLiveWave, B_CY - WAVE_TOP);

  int headX = dataSize ? (int)((uint64_t)played * CONTENT_W / dataSize) : 0;
  if (headX >= CONTENT_W) headX = CONTENT_W - 1;
  cv.drawFastVLine(headX, 0, WAVE_CY - WAVE_TOP, COL_WHITE);
}

static void drawPlaybackWaveDirect(m5gfx::M5GFX &g, uint32_t played, uint32_t dataSize, int16_t *liveWave, size_t liveN) {
  if (liveWave && liveN > 0) updateLiveWaveVisual(playbackLiveWave, liveWave, liveN, B_HALF, PB_B_SMOOTH_FAST, B_DECAY, PB_B_GAIN);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
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
  drawDseg14Text(g, 4, WAVE_BOT + 2, curBuf, recAccentColor());
  if (deleteHeldMs > 0) {
    drawDeleteProgress(g, deleteHeldMs);
  } else {
    char totBuf[9];
    snprintf(totBuf, sizeof(totBuf), "%02lu:%02lu", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    drawDseg14Text(g, CONTENT_W - dseg14TextWidth(totBuf) - 4, WAVE_BOT + 2, totBuf, recDimColor());
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

static uint8_t perfHudKindForKey(char key) {
  int8_t semis = 0;
  if (scaleNoteSemisForKey(key, semis)) return 1;
  if (isScaleSampleKey(key)) return 2;
  return 0;
}

static int perfHudSampleIndex(char key) {
  key = normalizeBindKey(key);
  const char *keys = "asdfghjklzxcvbnm";
  const char *p = strchr(keys, key);
  return p ? (int)(p - keys) : -1;
}

static int perfHudNoteIndex(char key) {
  key = normalizeBindKey(key);
  const char *keys = "qwertyu1234567890iop";
  const char *p = strchr(keys, key);
  return p ? (int)(p - keys) : -1;
}

static void perfHudNoteLabel(char key, char *out, size_t n) {
  if (!out || n == 0) return;
  int8_t semis = 0;
  if (!scaleNoteSemisForKey(key, semis)) {
    strlcpy(out, "---", n);
    return;
  }
  int total = (int)semis + (int)g_shortcutTransposeSemis;
  int octave = 4 + ((total >= 0) ? (total / 12) : -(((-total) + 11) / 12));
  int pc = total % 12;
  if (pc < 0) pc += 12;
  const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  snprintf(out, n, "%s%d", names[pc], octave);
}

static void perfHudLabelForKey(char key, int recNum, char *out, size_t n) {
  if (!out || n == 0) return;
  if (perfHudKindForKey(key) == 1) perfHudNoteLabel(key, out, n);
  else snprintf(out, n, "R%04d", recNum);
}

static void perfHudSourceLabel(int recNum, char *out, size_t n) {
  if (!out || n == 0) return;
  char hk = hotkeyOf(recNum);
  if (!hk && g_scaleCurrentRec > 0) hk = hotkeyOf(g_scaleCurrentRec);
  if (hk) snprintf(out, n, "SRC:%c", dispKey(hk));
  else strlcpy(out, "SRC:-", n);
}

static void perfHudKeyLabel(char *out, size_t n) {
  if (!out || n == 0) return;
  char root = g_shortcutKeyRoot ? g_shortcutKeyRoot : 'C';
  snprintf(out, n, "KEY:%c", root);
}

static void perfHudOctaveRootLabel(uint8_t band, char *out, size_t n) {
  if (!out || n == 0) return;
  char root = g_shortcutKeyRoot ? g_shortcutKeyRoot : 'C';
  snprintf(out, n, "%c%d", root, 3 + (int)band);
}

static bool perfHudScaleInfo(char key, const char **solfegeOut, const char **degreeOut, uint8_t *octaveBandOut, uint8_t *degreeIndexOut = nullptr) {
  key = normalizeBindKey(key);
  const char *solfege = nullptr;
  const char *degree = nullptr;
  uint8_t band = 1;
  uint8_t degreeIndex = 0;
  switch (key) {
    case 'q': case '1': case '8': solfege = "Do"; degree = "1"; degreeIndex = 0; break;
    case 'w': case '2': case '9': solfege = "Re"; degree = "2"; degreeIndex = 1; break;
    case 'e': case '3': case '0': solfege = "Mi"; degree = "3"; degreeIndex = 2; break;
    case 'r': case '4': case 'i': solfege = "Fa"; degree = "4"; degreeIndex = 3; break;
    case 't': case '5': case 'o': solfege = "So"; degree = "5"; degreeIndex = 4; break;
    case 'y': case '6': case 'p': solfege = "La"; degree = "6"; degreeIndex = 5; break;
    case 'u': case '7': solfege = "Ti"; degree = "7"; degreeIndex = 6; break;
    default: return false;
  }
  switch (key) {
    case 'q': case 'w': case 'e': case 'r': case 't': case 'y': case 'u':
      band = 0;
      break;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7':
      band = 1;
      break;
    default:
      band = 2;
      break;
  }
  if (solfegeOut) *solfegeOut = solfege;
  if (degreeOut) *degreeOut = degree;
  if (octaveBandOut) *octaveBandOut = band;
  if (degreeIndexOut) *degreeIndexOut = degreeIndex;
  return true;
}

static void drawPerfHudOctaveBand(m5gfx::M5GFX &g, int idx, bool active) {
  if (idx < 0 || idx > 2) return;
  const int w = 58;
  const int h = 8;
  const int gap = 9;
  const int x = 24 + idx * (w + gap);
  const int y = 120;
  char label[5];
  perfHudNoteLabel(idx == 0 ? 'q' : (idx == 1 ? '1' : '8'), label, sizeof(label));
  g.fillRect(x, y - 15, w, h + 15, COL_BG);
  int tw = (int)strlen(label) * 6;
  FONT_ASCII(g);
  g.setTextSize(1);
  g.setTextColor(recAccentColor(), COL_BG);
  g.setCursor(x + (w - tw) / 2, y - 15);
  g.print(label);
  g.drawRect(x, y, w, h, recAccentColor());
  if (active) g.fillRect(x + 2, y + 2, w - 4, h - 4, recAccentColor());
}

static void drawPerfHudScaleDetails(m5gfx::M5GFX &g, char key) {
  const char *solfege = "--";
  const char *degree = "-";
  uint8_t octaveBand = 1;
  uint8_t degreeIndex = 0;
  perfHudScaleInfo(key, &solfege, &degree, &octaveBand, &degreeIndex);
  g.fillRect(0, 40, CONTENT_W, 88, COL_BG);

  const int sideY = 58;
  drawDseg14TextScaled(g, 20, sideY, solfege, recAccentColor(), 1);
  int degreeX = CONTENT_W - dseg14TextWidthScaled(degree, 1) - 26;
  drawDseg14TextScaled(g, degreeX, sideY, degree, recAccentColor(), 1);

  const int groupCenters[3] = {39, 120, 201};
  const int dotGap = 9;
  const int dotY = 123;
  for (int band = 0; band < 3; band++) {
    char label[5];
    perfHudOctaveRootLabel((uint8_t)band, label, sizeof(label));
    int labelW = dseg14TextWidthScaled(label, 1);
    drawDseg14TextScaled(g, groupCenters[band] - labelW / 2, 98, label, recAccentColor(), 1);
    int startX = groupCenters[band] - dotGap * 3;
    for (int note = 0; note < 7; note++) {
      bool active = (band == octaveBand && note == degreeIndex);
      int x = startX + note * dotGap;
      if (active) g.fillCircle(x, dotY, 3, recAccentColor());
      else g.fillCircle(x, dotY, 1, recAccentColor());
    }
  }
}

static void drawPerfHudSampleCell(m5gfx::M5GFX &g, int idx, bool on) {
  if (idx < 0 || idx >= 16) return;
  const char *keys = "ASDFGHJKLZXCVBNM";
  int row = (idx < 9) ? 0 : 1;
  int col = (idx < 9) ? idx : (idx - 9);
  const int cellW = 24;
  const int row0X = 12;
  const int row1X = row0X + cellW;
  const int x = (row == 0 ? row0X : row1X) + col * cellW;
  const int y = 84 + row * 22;
  char label[2] = {keys[idx], 0};
  char bindKey = normalizeBindKey(keys[idx]);
  bool bound = bindKey && findHotkey(bindKey) > 0;
  uint16_t colText = on ? recAccentColor() : (bound ? recDimColor() : COL_DARK_GRAY);
  int w = dseg14TextWidthScaled(label, 1);
  g.fillRect(x, y, cellW, 18, COL_BG);
  drawDseg14TextScaled(g, x + (cellW - w) / 2, y + 1, label, colText, 1);
}

static void drawPerfHudNoteCell(m5gfx::M5GFX &g, int idx, bool on) {
  if (idx < 0 || idx >= 20) return;
  int x = 8 + idx * 11;
  int y = 111;
  g.fillRect(x, y, 8, on ? 14 : 8, on ? recAccentColor() : COL_DARK_GRAY);
}

static void drawPerfHudBase(m5gfx::M5GFX &g, uint8_t kind) {
  g.fillScreen(COL_BG);
  if (g_scaleMode) {
    drawDseg14TextScaled(g, CONTENT_W - dseg14TextWidthScaled("SCALE", 1) - 4, 1, "SCALE", recAccentColor(), 1);
  } else {
    drawDseg14TextScaled(g, 4, 1, shortcutGroupLabel(), recAccentColor(), 1);
  }
  if (kind == 1) {
    if (!g_scaleMode) drawDseg14TextScaled(g, CONTENT_W - dseg14TextWidthScaled("NOTES", 1) - 4, 1, "NOTES", recAccentColor(), 1);
    g.fillRect(18, 40, 204, 90, COL_BG);
  } else {
    for (int i = 0; i < 16; i++) drawPerfHudSampleCell(g, i, false);
    if (!g_scaleMode) drawDseg14TextScaled(g, CONTENT_W - dseg14TextWidthScaled("KEYS", 1) - 4, 1, "KEYS", recAccentColor(), 1);
  }
}

static void drawPerfHudKey(m5gfx::M5GFX &g, char key, int recNum) {
  key = normalizeBindKey(key);
  if (!key) return;
  uint8_t kind = perfHudKindForKey(key);
  if (!g_perfHudVisible || g_perfHudLastKind != kind) {
    drawPerfHudBase(g, kind);
    g_perfHudVisible = true;
    g_perfHudLastKey = 0;
    g_perfHudLastKind = kind;
  }
  if (g_perfHudLastKey && g_perfHudLastKey != key) {
    if (g_perfHudLastKind == 2) drawPerfHudSampleCell(g, perfHudSampleIndex(g_perfHudLastKey), false);
  }
  if (kind == 2) drawPerfHudSampleCell(g, perfHudSampleIndex(key), true);

  if (g_scaleMode) {
    char source[8];
    perfHudSourceLabel(recNum, source, sizeof(source));
    g.fillRect(0, 0, 72, 18, COL_BG);
    drawDseg14TextScaled(g, 4, 1, source, recAccentColor(), 1);
  }

  if (g_scaleMode && kind == 1) drawPerfHudScaleDetails(g, key);

  char label[8];
  perfHudLabelForKey(key, recNum, label, sizeof(label));
  const int labelScale = 2;
  int w = dseg14TextWidthScaled(label, labelScale);
  int x = ((CONTENT_W - w) / 2);
  x -= x % labelScale;
  int labelY = (g_scaleMode && kind == 1) ? 42 : 34;
  if (g_scaleMode && kind == 1) g.fillRect(70, 26, 100, 42, COL_BG);
  else g.fillRect(30, 26, 180, 48, COL_BG);
  drawDseg14TextScaled(g, x, labelY, label, recAccentColor(), labelScale);

  g_perfHudLastKey = key;
}

static bool enqueueUploadMounted(int recNum, uint8_t kind);
static bool uploadCancelMounted(int recNum = 0);
static uint8_t validateUploadConfigMounted();
static const char *uploadStatusLabel(uint8_t status);
static bool extractJsonStringValue(const String &json, const char *key, char *out, size_t n);

// ---------- 鍥炴斁鐣岄潰: 鎾斁瀹岃嚜鍔ㄥ洖鍒楄〃, 鍥炶溅鏆傚仠/缁х画, 閫€鏍煎洖鍒楄〃, ;/.鍒囨崲, +/- 闊抽噺, 闀挎寜Del鍒犻櫎, 绌烘牸鍘诲綍闊?----------
// 杩斿洖 R_BACK(杩斿洖涓婁竴灞?, R_LIST(鍥炲垪琛?, R_RECORD(鍘诲綍闊? 鎴?R_DELETE(鍒犻櫎褰撳墠褰曢煶)
int playbackScreen(const char *path, int recNum, int prevRec, int nextRec) {
  pauseUploadForMedia();
  auto &d = M5Cardputer.Display;
  if (!sdMount()) { showMsg("鎾斁", "SD 璇诲彇澶辫触", COL_RED); return R_LIST; }
  bool isHotkeyPlayback = hotkeyOf(recNum) != 0 || (g_scaleMode && recNum == g_scaleCurrentRec);
  File f = SD.open(path, FILE_READ);
  if (!f) { SD.end(); showMsg("鎾斁", "鎵撲笉寮€鏂囦欢", COL_RED); return R_LIST; }
  uint32_t total = f.size();
  if (total <= 44) { f.close(); SD.end(); showMsg("鎾斁", "鏂囦欢涓虹┖", COL_RED); return R_LIST; }
  WavInfo wav;
  if (!parseWavInfo(f, wav)) {
    const char *err = wav.error ? wav.error : "BAD WAV";
    f.close(); SD.end(); showMsg("PLAY", err, COL_RED); return R_LIST;
  }
  uint32_t dataSize = wav.dataBytes;
  uint32_t sourceRate = wav.sampleRate;
  const uint16_t wavChannels = wav.channels;
  const uint16_t wavBlockAlign = wav.blockAlign;
  const bool wavStereo = wavChannels == 2;
  float shortcutRootSemis = activeShortcutSemis();
  uint32_t playbackRate = shortcutPlaybackRate(sourceRate, isHotkeyPlayback);
  bool hotkeyShortBoost = isHotkeyPlayback && dataSize < (sourceRate * wavBlockAlign);
  bool pitchBendEnabled = beginShortcutPitchBend(isHotkeyPlayback);
  bool hotkeyPerfMode = isHotkeyPlayback;
  char perfHudKey = g_perfHudPendingKey;
  g_perfHudPendingKey = 0;
  if (!perfHudKey) perfHudKey = pressedBindKey();
  if (!perfHudKey) perfHudKey = hotkeyOf(recNum);
  if (!hotkeyPerfMode) g_perfHudVisible = false;
  f.seek(wav.dataOffset);
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(playbackLiveWave, 0, sizeof(playbackLiveWave));

  if (!hotkeyPerfMode) d.fillScreen(COL_BG);
  M5Canvas waveCv(&d);
  waveCv.setColorDepth(8);
  bool useWaveSprite = !hotkeyPerfMode && waveCv.createSprite(CONTENT_W, WAVE_H) != nullptr;
  M5Canvas bottomCv(&d);
  bottomCv.setColorDepth(8);
  bool useBottomSprite = !hotkeyPerfMode && useWaveSprite && bottomCv.createSprite(CONTENT_W, CHROME_H) != nullptr;
  M5Canvas cv(&d);
  cv.setColorDepth(8);
  bool useSprite = !hotkeyPerfMode && !useWaveSprite && cv.createSprite(CONTENT_W, d.height()) != nullptr;
  const uint32_t pbFrameMs = (useWaveSprite || useSprite) ? PB_UI_FRAME_MS : DIRECT_UI_FRAME_MS;
  g_mediaBusy = true;

  // 闈欐€侀儴鍒?鐢讳竴娆?: 椤舵爮
  auto drawStatic = [&]() {
    if (hotkeyPerfMode) return;
    // 鏂囦欢缂栧彿 (宸︿笂灏忓瓧)
    char title[16];
    if (isMusicRec(recNum)) musicListLabel(recNum, title, sizeof(title));
    else snprintf(title, sizeof(title), "REC_%04d", recNum);
    char hk = hotkeyOf(recNum);
    if (useSprite) {
      cv.fillScreen(COL_BG);
      drawStatusTitleNoLine(cv, title, hk ? recDimColor() : recAccentColor());
      if (hk) {
        char keyLabel[8];
        snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
        drawDseg14Text(cv, dseg14TextWidth(title) + 10, 1, keyLabel, recAccentColor());
      }
      cv.drawFastHLine(0, WAVE_BOT + 1, CONTENT_W, 0x0820);
    } else {
      d.fillScreen(COL_BG);
      drawStatusTitleNoLine(d, title, hk ? recDimColor() : recAccentColor());
      if (hk) {
        char keyLabel[8];
        snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
        drawDseg14Text(d, dseg14TextWidth(title) + 10, 1, keyLabel, recAccentColor());
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
    if (hotkeyPerfMode) {
      (void)played;
      (void)liveWave;
      (void)liveN;
      return;
    }
    uint32_t chromeSec = durationFromPcm(played, sourceRate, wavChannels);
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
  uint32_t playbackEdgeFadeBytes = sourceRate * wavBlockAlign * 80 / 1000;
  uint32_t fadeBytesLeft = playbackEdgeFadeBytes;
  int32_t pbDc = 0;
  int32_t cassetteLp = 0;
  bool shiftedShortcutKey = isHotkeyPlayback && activeShortcutSemis() != 0.0f;
  int32_t keySizzleLp = 0;
  int32_t keySizzlePrev = 0;
  uint8_t previewBar = 0;
  int pi = 0;

  drawStatic();
  drawProgress(0);
  lastProgressDraw = millis();
  speakerDrainBeforeNextPlayback = false;
  bool seamlessStart = isHotkeyPlayback && g_seamlessHotkeySwitch && speakerOutputReady;
  g_seamlessHotkeySwitch = false;
  if (seamlessStart) {
    playbackEdgeFadeBytes = REC_RATE * 2 * 18 / 1000;
    fadeBytesLeft = playbackEdgeFadeBytes;
    speakerCancelIdleOff();
    speakerApplyOutputRoute();
    speakerSetVolume(playbackVolumeForRecMode(recNum, isHotkeyPlayback));
  } else {
    speakerOn(true);
    if (isHotkeyPlayback) speakerPrimeAfterColdStart();
    speakerSetVolume(playbackVolumeForRecMode(recNum, isHotkeyPlayback));
  }
  if (hotkeyPerfMode) drawPerfHudKey(d, perfHudKey, recNum);
  bool ignoreKeysUntilRelease = M5Cardputer.Keyboard.isPressed();
  char ignoreStartBindKey = pressedBindKey();
  auto handleFastPlaybackKey = [&]() -> bool {
    if (keyCtrl() || keyTab() || keyFn() || keyAlt() || keyEnter() || keyDel() || keySpace() || keyUpload()) return false;
    char currentBind = ignoreKeysUntilRelease ? pressedBindKeyExcept(ignoreStartBindKey) : pressedBindKey();
    if (!currentBind) return false;
    if (scaleModeCanHandleKey()) {
      int8_t noteSemis = 0;
      if (scaleNoteSemisForKey(currentBind, noteSemis)) {
        g_scaleCurrentRec = recNum;
        if (prepareScaleNoteForRec(recNum, noteSemis)) {
          g_seamlessHotkeySwitch = true;
          g_perfHudPendingKey = currentBind;
          g_nextPlay = recNum;
          ret = R_PLAY;
          stop = true;
          return true;
        }
      } else if (isScaleSampleKey(currentBind)) {
        int sampleRec = findHotkey(currentBind);
        if (sampleRec > 0) {
          g_scaleCurrentRec = sampleRec;
          clearShortcutSemisOverride();
          selectHotkeyPreviewRec(sampleRec);
          g_seamlessHotkeySwitch = true;
          g_perfHudPendingKey = currentBind;
          g_nextPlay = sampleRec;
          ret = R_PLAY;
          stop = true;
          return true;
        }
      }
    }
    int hk = findHotkey(currentBind);
    if (hk > 0) {
      g_seamlessHotkeySwitch = true;
      g_listMode = REC_SHORTCUT;
      g_perfHudPendingKey = currentBind;
      g_nextPlay = hk;
      ret = R_PLAY;
      stop = true;
      return true;
    }
    return false;
  };

  while (!stop) {
    M5Cardputer.update();
    if (ignoreKeysUntilRelease) {
      if (!M5Cardputer.Keyboard.isPressed()) ignoreKeysUntilRelease = false;
      else if (handleFastPlaybackKey()) break;
      else {
        char currentBind = pressedBindKey();
        if (currentBind && currentBind != ignoreStartBindKey) ignoreKeysUntilRelease = false;
      }
    } else {
      if (handleFastPlaybackKey()) break;
      if (keyDel()) {
        if (isHotkeyPlayback) {
          drawPlaybackAction(cv, "STOP", recDimColor());
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
        drawPlaybackAction(cv, "BACK", recDimColor());
        ret = R_LIST; stop = true;                         // 閫€鏍肩煭鎸?鍥炰笂涓€灞?
      }
      if (stop) break;

      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyRollback()) { drawPlaybackAction(cv, "ROLL", recAccentColor()); ret = R_ROLLBACK; stop = true; waitRelease(); break; }
        if (handleScaleModeChord()) {
          drawPlaybackAction(cv, g_scaleMode ? "SCALE" : "NORM", g_scaleMode ? recAccentColor() : recDimColor());
          waitRelease();
        }
        else if (handleShortcutGroupChord()) {
          drawPlaybackAction(cv, shortcutGroupLabel(), recAccentColor());
          waitRelease();
        }
        else if (handleShortcutKeyRootChord()) {
          drawPlaybackAction(cv, shortcutKeyRootLabel(), recAccentColor());
          waitRelease();
        }
        else {
        if (scaleModeCanHandleKey()) {
          int8_t noteSemis = 0;
          if (scaleNoteSemis(noteSemis)) {
            g_scaleCurrentRec = recNum;
            if (prepareScaleNoteForRec(recNum, noteSemis)) {
              g_seamlessHotkeySwitch = true;
              g_perfHudPendingKey = pressedBindKey();
              g_nextPlay = recNum;
              ret = R_PLAY;
              stop = true;
            }
          } else {
            char scaleKey = pressedBindKey();
            if (isScaleSampleKey(scaleKey)) {
              int sampleRec = findHotkey(scaleKey);
              if (sampleRec > 0) {
                g_scaleCurrentRec = sampleRec;
                clearShortcutSemisOverride();
                selectHotkeyPreviewRec(sampleRec);
                g_seamlessHotkeySwitch = true;
                g_perfHudPendingKey = scaleKey;
                g_nextPlay = sampleRec;
                ret = R_PLAY;
                stop = true;
              } else {
                drawPlaybackAction(cv, "NO SMP", recDimColor());
                waitRelease();
              }
            }
          }
          if (stop) break;
        }
        int hk = pressedHotkeyRec();
        if (hk > 0) {
          drawPlaybackAction(cv, "PLAY", recAccentColor());
          g_seamlessHotkeySwitch = true;
          g_listMode = REC_SHORTCUT;
          g_perfHudPendingKey = pressedBindKey();
          g_nextPlay = hk; ret = R_PLAY; stop = true;
        }   // 鎸夊埌鍒殑缁戝畾閿?绔嬪埢瑕嗙洊鎾斁
        else if (keyEsc()) { drawPlaybackAction(cv, "SLEEP", recDimColor()); ret = R_BACK; stop = true; }       // Esc=鎭睆
        else if (keySpace()) { drawPlaybackAction(cv, "REC", COL_RED); ret = R_RECORD; stop = true; }   // 绌烘牸=鍘诲綍闊?
        else if (keyUp() && prevRec > 0) { drawPlaybackAction(cv, "PREV", recAccentColor()); speakerQuietFlush(6); g_nextPlay = prevRec; ret = R_PLAY; stop = true; }
        else if (keyDown() && nextRec > 0) { drawPlaybackAction(cv, "NEXT", recAccentColor()); speakerQuietFlush(6); g_nextPlay = nextRec; ret = R_PLAY; stop = true; }
        else if (keyFn() && keyUpload()) {
          uint8_t status = UPSTAT_IDLE;
          bool cancelled = uploadCancelMounted(recNum);
          if (cancelled) status = UPSTAT_ABORTED;
          g_uploadStatus = status;
          drawPlaybackAction(cv, cancelled ? "ABORT" : (status == UPSTAT_NO_SD ? "NO SD" : "NO WT"), cancelled ? recAccentColor() : recDimColor());
        }
        else if (keyUpload()) {
          uint8_t status = UPSTAT_IDLE;
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
          g_uploadStatus = status;
          drawPlaybackAction(cv, uploadStatusLabel(status), (status == UPSTAT_QUEUED || status == UPSTAT_DONE) ? recAccentColor() : COL_RED);
        }
        else if (keyEnter()) {
          paused = !paused;
          if (paused) speakerSetVolume(0);
          else fadeInPlaybackVolumeForRec(recNum);
          drawProgress(played);
          lastProgressDraw = millis();
          drawPlaybackAction(cv, paused ? "PAUSE" : "PLAY", paused ? recDimColor() : recAccentColor());
          waitRelease();
        }
        else if (keyVolUp()) {
          if (keyAlt()) {
            adjustRecVolumeDelta(recNum, 1);
            applyPlaybackVolumeForRec(recNum);
            drawPlaybackAction(cv, "V+", recAccentColor());
          } else {
            adjustPlayVolume(25);
            applyPlaybackVolumeForRec(recNum);
            if (useSprite) drawCanvasVolumeToast(cv, recAccentColor());
            else drawVolumeToast(recAccentColor());
          }
        }
        else if (keyVolDn()) {
          if (keyAlt()) {
            adjustRecVolumeDelta(recNum, -1);
            applyPlaybackVolumeForRec(recNum);
            drawPlaybackAction(cv, "V-", recAccentColor());
          } else {
            adjustPlayVolume(-25);
            applyPlaybackVolumeForRec(recNum);
            if (useSprite) drawCanvasVolumeToast(cv, recAccentColor());
            else drawVolumeToast(recAccentColor());
          }
        }
        else if (keyFn() && (keyBrightUp() || keyBrightDn())) {
          cycleRecAccent(keyBrightUp() ? 1 : -1);
          saveRecAccentState();
          drawProgress(played);
          drawPlaybackAction(cv, "COLOR", recAccentColor());
          waitRelease();
        }
        else if (keyBrightUp()) {
          adjustBrightness(1);
          if (useSprite) drawCanvasBrightnessToast(cv, recAccentColor());
          else drawBrightnessToast(recAccentColor());
        }
        else if (keyBrightDn()) {
          adjustBrightness(-1);
          if (useSprite) drawCanvasBrightnessToast(cv, recAccentColor());
          else drawBrightnessToast(recAccentColor());
        }
        }
      }
    }
    if (stop) break;
    if (paused) { delay(8); continue; }

    bool seekLeft = keyLeft();
    bool seekRight = keyRight();
    if ((seekLeft || seekRight) && millis() - lastSeek > 120) {
      lastSeek = millis();
      speakerSetVolume(0);
      uint32_t step = sourceRate * wavBlockAlign;  // about one second of PCM data
      if (seekLeft) played = (played > step) ? (played - step) : 0;
      if (seekRight) played = (played + step < dataSize) ? (played + step) : dataSize;
      played -= played % wavBlockAlign;
      remaining = dataSize - played;
      f.seek(wav.dataOffset + played);
      playbackVisualN = 0;
      memset(playbackLiveWave, 0, sizeof(playbackLiveWave));
      playbackEdgeFadeBytes = PLAYBACK_EDGE_FADE_BYTES;
      fadeBytesLeft = playbackEdgeFadeBytes;
      pbDc = 0;
      cassetteLp = 0;
      shortcutRootSemis = activeShortcutSemis();
      keySizzleLp = 0;
      keySizzlePrev = 0;
      drawProgress(played);
      lastProgressDraw = millis();
      playSeekFeedback(seekRight && !seekLeft);
      fadeInPlaybackVolumeForRec(recNum);
      delay(8);
      continue;
    }
    if (remaining == 0) {
      if (M5Cardputer.Speaker.isPlaying(0) == 0) {
        if (!playDone) {
          playDone = true;
          drawProgress(dataSize);   // 鏄剧ず 100% 鏈€缁堢姸鎬?
        }
        if (isHotkeyPlayback) {
          uint32_t waitStart = millis();
          while (millis() - waitStart < HOTKEY_END_HOLD_MS) {
            M5Cardputer.update();
            if (handleFastPlaybackKey()) break;
            if (!M5Cardputer.Keyboard.isPressed()) ignoreKeysUntilRelease = false;
            delay(1);
          }
          if (stop) break;
        } else {
          delay(80);
        }
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
        previewPlaybackWaveStep(f, wav.dataOffset, dataSize, wavBlockAlign, previewBar, 1);
        drawProgress(played);
        lastProgressDraw = lastPreviewDraw;
      }
      delay(1);
      continue;
    }
    size_t want = remaining < sizeof(pbBuf[pi]) ? remaining : sizeof(pbBuf[pi]);
    want -= want % wavBlockAlign;
    if (want == 0) { remaining = 0; continue; }
    if (!f.seek(wav.dataOffset + played)) { remaining = 0; continue; }
    int got = f.read((uint8_t *)pbBuf[pi], want);
    if (got <= 0) { remaining = 0; continue; }
    got -= got % wavBlockAlign;
    if (got <= 0) { remaining = 0; continue; }
    remaining -= got;
    int16_t *liveWave = pbBuf[pi];
    size_t liveN = got / 2;
    uint32_t chunkStart = played;
    if (isHotkeyPlayback) {
      float targetRootSemis = activeShortcutSemis();
      shortcutRootSemis += (targetRootSemis - shortcutRootSemis) * 0.18f;
      if (fabsf(targetRootSemis - shortcutRootSemis) < 0.015f) shortcutRootSemis = targetRootSemis;
      playbackRate = shortcutRateForSemis(sourceRate, shortcutRootSemis);
    }
    uint32_t livePlaybackRate = shortcutMotionRate(playbackRate, pitchBendEnabled);
    if (isHotkeyPlayback) {
      for (size_t i = 0; i < liveN; i++) {
        int32_t raw = liveWave[i];
        pbDc += (raw - pbDc) >> 8;
        int32_t sample = raw - pbDc;
        int32_t gain = 256;
        if (fadeBytesLeft > 0) {
          uint32_t done = playbackEdgeFadeBytes - fadeBytesLeft;
          int32_t inGain = (int32_t)(done * 256 / playbackEdgeFadeBytes);
          if (inGain < gain) gain = inGain;
          fadeBytesLeft = (fadeBytesLeft > 2) ? (fadeBytesLeft - 2) : 0;
        }
        uint32_t sampleByte = chunkStart + (uint32_t)i * 2;
        uint32_t bytesToEnd = (sampleByte < dataSize) ? (dataSize - sampleByte) : 0;
        if (bytesToEnd < playbackEdgeFadeBytes) {
          int32_t outGain = (int32_t)(bytesToEnd * 256 / playbackEdgeFadeBytes);
          if (outGain < gain) gain = outGain;
        }
        if (pitchBendEnabled && g_shortcutTailGain < 0.995f) {
          gain = (int32_t)((float)gain * g_shortcutTailGain);
        }
        sample = (sample * gain) >> 8;
        cassetteLp += (sample - cassetteLp) >> 3;
        sample = (sample * 7 + cassetteLp) >> 3;
        int32_t drive = sample + (sample >> 4);
        if (hotkeyShortBoost) drive += drive >> 1;
        if (drive > 30000) drive = 30000 + ((drive - 30000) >> 3);
        if (drive < -30000) drive = -30000 + ((drive + 30000) >> 3);
        sample = drive;
        if (shiftedShortcutKey) {
          keySizzleLp += (sample - keySizzleLp) >> 2;
          int32_t hi = sample - keySizzleLp;
          int32_t diff = abs((int)(sample - keySizzlePrev));
          int32_t hiMixQ8 = diff > 4200 ? 150 : (diff > 2600 ? 184 : 218);
          sample = keySizzleLp + ((hi * hiMixQ8) >> 8);
          keySizzlePrev = sample;
        }
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        liveWave[i] = (int16_t)sample;
      }
    }
    M5Cardputer.Speaker.playRaw(liveWave, liveN, livePlaybackRate, wavStereo, 1, 0, false);
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
  if (ret == R_PLAY) {
    speakerCancelIdleOff();
    speakerDrainBeforeNextPlayback = false;
  } else if (ret == R_LIST) {
    speakerScheduleIdleOff();
    if (hotkeyPerfMode) g_perfHudVisible = false;
  } else {
    speakerSoftStop(12);
    if (hotkeyPerfMode) g_perfHudVisible = false;
  }
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
static int rollbackRecordScreen();
static void deleteRecording(int recNum);

// 鎾斁娴佺▼: 鏀寔"鎾斁涓寜鍒殑缁戝畾閿?-> 绔嬪埢鍒囧埌閭ｆ潯"(瑕嗙洊). 杩斿洖 R_BACK/R_LIST/R_RECORD.
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
    char p[128]; recordingPathForRec(recNum, p, sizeof(p));
    int r = playbackScreen(p, recNum, prevRec, nextRec);
    if (r == R_PLAY) { recNum = g_nextPlay; continue; }
    if (r == R_DELETE) { g_listReturnRec = 0; deleteRecording(recNum); return R_LIST; }
    if (r == R_ROLLBACK) return R_ROLLBACK;
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
      char p[128]; recordingPathForRec(recNum, p, sizeof(p));
      noiseReduce(p);
      action = R_LIST;
      continue;
    }
    listFlow(recNum);
    return;
  }
}

// ---------- 绯荤粺灞備笂浼犳湇鍔? 褰曢煶搴旂敤鍙叆闃? 鐩掑瓙璐熻矗 Wi-Fi / 璋冨害 / 閲嶈瘯 ----------
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
      if (k >= REC_NORMAL && k < REC_KIND_COUNT) kind = (uint8_t)k;
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
  if (!g_wifiSessionActive && WiFi.getMode() == WIFI_OFF) {
    boxClockSynced = false;
    return;
  }
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(60);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_wifi_deinit();
  delay(140);
  g_wifiSessionActive = false;
  boxClockSynced = false;
}

static void pauseUploadForMedia() {
  g_uploadActiveAnnounced = false;
  g_uploadActiveRec = 0;
  if (g_uploadStatus == UPSTAT_UPLOADING) g_uploadStatus = UPSTAT_QUEUED;
#if UPLOAD_WIFI_ENABLED
  if (g_wifiSessionActive || WiFi.getMode() != WIFI_OFF) wifiPowerDown();
#endif
}

static bool tryWifiProfile(const char *ssid, const char *password) {
  if (!ssid || !ssid[0]) return false;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(120);
  WiFi.mode(WIFI_STA);
  g_wifiSessionActive = true;
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
  char path[128];
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
    if (keyUploadAbort()) {
      g_uploadPausedForInput = true;
      g_uploadStatus = UPSTAT_QUEUED;
      break;
    }
    int nextRec = 0;
    uint8_t nextKind = REC_NORMAL;
    if (g_uploadModeVisible && uploadReadFirstJob(nextRec, nextKind)) {
      g_uploadActiveRec = nextRec;
      g_uploadStatus = UPSTAT_UPLOADING;
      drawUploadMode("GO", recAccentColor());
    }
    bool oneChanged = uploadOneJobMounted();
    if (!oneChanged) break;
    if (g_uploadBatchDone < 255) g_uploadBatchDone++;
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

static uint8_t uploadQueueCountMounted() {
  uint8_t count = 0;
  File f = SD.open(UPLOAD_QUEUE_PATH, FILE_READ);
  if (!f) return 0;
  char line[32];
  while (f.available() && count < 255) {
    int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[len] = 0;
    strTrim(line);
    if (line[0]) count++;
  }
  f.close();
  return count;
}

static void drawUploadMode(const char *status, uint16_t col) {
  auto &d = M5Cardputer.Display;
  uint16_t accent = (col == COL_RED) ? COL_RED : recAccentColor();
  d.fillScreen(COL_BG);
  drawStatusTitleNoLine(d, "UPLOAD", accent);
  d.drawFastHLine(0, WAVE_TOP - 1, CONTENT_W, recAxisColor());

  int statusScale = strlen(status) > 6 ? 1 : 2;
  int statusW = dseg14TextWidthScaled(status, statusScale);
  int statusX = (CONTENT_W - statusW) / 2;
  if (statusX < 4) statusX = 4;
  drawDseg14TextScaled(d, statusX, 42, status, accent, statusScale);

  int infoY = 82;
  if (g_uploadActiveRec > 0) {
    char rec[16];
    snprintf(rec, sizeof(rec), "REC%04d", g_uploadActiveRec);
    drawDseg14Text(d, 4, infoY, rec, recAccentColor());
  }
  if (g_uploadBatchTotal > 0) {
    char progress[16];
    uint8_t current = g_uploadBatchDone;
    if (strcmp(status, "GO") == 0 && current < g_uploadBatchTotal) current++;
    snprintf(progress, sizeof(progress), "%02u/%02u", (unsigned)current, (unsigned)g_uploadBatchTotal);
    drawDseg14Text(d, CONTENT_W - dseg14TextWidth(progress) - 4, infoY, progress, recDimColor());
  }

  d.drawFastHLine(0, 113, CONTENT_W, recAxisColor());
  drawDseg14Text(d, 4, 116, "SPC REC", recDimColor());
  drawDseg14Text(d, 94, 116, "ENT LIST", recDimColor());
  drawDseg14Text(d, CONTENT_W - dseg14TextWidth("ESC") - 4, 116, "ESC", recDimColor());
}

static bool runUploadBatchMounted(bool visible) {
#if !UPLOAD_WIFI_ENABLED
  (void)visible;
  return false;
#else
  g_uploadModeActive = true;
  g_uploadModeVisible = visible;
  g_uploadModeIgnoreKeys = visible && (M5Cardputer.Keyboard.isPressed() || keyGo());
  g_uploadExitRequested = false;
  g_uploadExitApp = APP_SLEEP;
  g_uploadBatchDone = 0;
  g_uploadBatchTotal = 0;
  if (!sdMount()) {
    g_uploadStatus = UPSTAT_NO_SD;
    if (visible) drawUploadMode(uploadStatusLabel(g_uploadStatus), COL_RED);
    g_uploadModeActive = false;
    g_uploadModeIgnoreKeys = false;
    return false;
  }
  int firstRec = 0;
  if (!uploadQueueHasJobMounted(&firstRec)) {
    g_uploadActiveAnnounced = false;
    g_uploadActiveRec = 0;
    g_uploadStatus = UPSTAT_IDLE;
    SD.end();
    if (visible) drawUploadMode("NO WT", recDimColor());
    g_uploadModeActive = false;
    g_uploadModeIgnoreKeys = false;
    return false;
  }
  g_uploadBatchTotal = uploadQueueCountMounted();
  g_uploadActiveAnnounced = visible;
  g_uploadActiveRec = firstRec;
  g_uploadStatus = UPSTAT_UPLOADING;
  if (g_uploadModeVisible) drawUploadMode("GO", recAccentColor());
  bool changed = uploadQueuedJobsMounted(UPLOAD_BATCH_MAX_JOBS, UPLOAD_BATCH_BUDGET_MS);
  int nextRec = 0;
  bool hasNext = uploadQueueHasJobMounted(&nextRec);
  g_uploadActiveAnnounced = false;
  g_uploadActiveRec = hasNext ? nextRec : 0;
  if (hasNext) {
    g_uploadStatus = UPSTAT_QUEUED;
    if (g_uploadModeVisible) drawUploadMode("WT", recAccentColor());
  } else if (changed && g_uploadStatus == UPSTAT_UPLOADING) {
    g_uploadStatus = UPSTAT_DONE;
    if (g_uploadModeVisible) drawUploadMode("OKK", recAccentColor());
  } else if (g_uploadModeVisible) {
    drawUploadMode(uploadStatusLabel(g_uploadStatus), (g_uploadStatus == UPSTAT_DONE || g_uploadStatus == UPSTAT_QUEUED) ? recAccentColor() : COL_RED);
  }
  SD.end();
  g_uploadModeActive = false;
  g_uploadModeIgnoreKeys = false;
  return changed;
#endif
}

static bool uploadModeHoldTriggered() {
  uint32_t start = millis();
  while (keyUpload() && !keyTab() && !keyFn()) {
    M5Cardputer.update();
    if (millis() - start >= UPLOAD_MODE_HOLD_MS) return true;
    delay(10);
  }
  return false;
}

static bool systemIdleTick() {
  speakerMaybeIdleOff();
  return false;
}

// ---------- 鎸夐敭鎽╂摝鍚庡鐞? 鍙帇浣庝綆鑳介噺銆侀珮鍙樺寲鐜囩殑缁嗙鍒摝澹?----------
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
  char p[128];
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

// ---------- 闄嶅櫔: FFT 璋卞噺娉?鍚庡鐞? ----------
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
  drawHeader("闄嶅櫔澶勭悊涓?..");

  if (!sdMount()) { showMsg("闄嶅櫔", "SD 璇诲彇澶辫触", COL_RED); return; }
  File in = SD.open(path, FILE_READ);
  if (!in || in.size() <= 44) { if (in) in.close(); SD.end(); showMsg("闄嶅櫔", "鏂囦欢鏃犳晥", COL_RED); return; }
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
  if (!out) { in.close(); SD.end(); showMsg("DENOISE", "WRITE ERR", COL_RED); return; }
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
      d.setTextColor(recAccentColor(), COL_BG);
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)((uint64_t)processed * 100 / dataBytes));
      drawDseg14Text(d, 8, 38, pctBuf, recAccentColor());
    }
  }
  writeWavHeader(out, REC_RATE, outBytes);
  out.flush(); out.close();
  in.close();
  replaceFileWithTemp(path, tmp);
  SD.end();
}

// ---------- 褰曢煶鐢婚潰 (canvas: CONTENT_W 脳 135, 鎺ㄩ€佸埌 x=0) ----------
// A绾? waveBars[] 灏忔煴杞ㄩ亾(涓婂崐鍖?  B绾? wave[]瀹炴椂绀烘尝鍣?涓嬪崐鍖?
void drawRecCanvas(M5Canvas &cv, uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);

  cv.fillScreen(COL_BG);

  // 鈹€鈹€ 椤舵爮 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
  const char *status = ready ? "READY" : (paused ? "PAUSE" : "REC");
  uint16_t statusCol = (ready || paused) ? recDimColor() : recAccentColor();
  drawStatusTitleNoLine(cv, status, statusCol);
  if (!ready && !paused) {
    if (blink) cv.fillCircle(48, 9, 5, COL_RED);
    else cv.drawCircle(48, 9, 5, 0x2000);
  }
  // 鐢甸噺 (浜豢, 榛戝簳纭繚鍙)

  // 鈹€鈹€ A绾? 婊氬姩鍘嗗彶 (涓婂崐鍖? 涓酱 y=39) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
  cv.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(cv);

  // 鈹€鈹€ B绾? 瀹炴椂绀烘尝鍣?(涓嬪崐鍖? 涓酱 y=85) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
  cv.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(cv, recLiveWave, B_CY);

  // 鈹€鈹€ 璁℃椂鍣?(搴曢儴宸︿晶) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
  uint32_t s = elapsedMs / 1000;
  cv.setTextColor(recAccentColor(), COL_BG);
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(cv, 4, WAVE_BOT + 2, recTime, recAccentColor());
  drawDeleteProgress(cv, deleteHeldMs);
}

void drawRecCanvas(m5gfx::M5GFX &g, uint32_t elapsedMs, bool blink, int16_t *wave, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);

  g.fillScreen(COL_BG);

  const char *status = ready ? "READY" : (paused ? "PAUSE" : "REC");
  uint16_t statusCol = (ready || paused) ? recDimColor() : recAccentColor();
  drawStatusTitleNoLine(g, status, statusCol);
  if (!ready && !paused) {
    if (blink) g.fillCircle(48, 9, 5, COL_RED);
    else g.drawCircle(48, 9, 5, 0x2000);
  }

  g.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(g);

  g.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(g, recLiveWave, B_CY);

  uint32_t s = elapsedMs / 1000;
  g.setTextColor(recAccentColor(), COL_BG);
  char recTime[8];
  snprintf(recTime, sizeof(recTime), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  drawDseg14Text(g, 4, WAVE_BOT + 2, recTime, recAccentColor());
  drawDeleteProgress(g, deleteHeldMs);
}

static void drawRecWaveCanvas(M5Canvas &cv, int16_t *wave) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);
  cv.fillScreen(COL_BG);
  cv.drawFastHLine(0, A_CY - WAVE_TOP, CONTENT_W, recAxisColor());
  drawTrackBarsLocal(cv);
  cv.drawFastHLine(0, B_CY - WAVE_TOP, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(cv, recLiveWave, B_CY - WAVE_TOP);
}

static void drawRecWaveDirect(m5gfx::M5GFX &g, int16_t *wave) {
  updateLiveWaveVisual(recLiveWave, wave, REC_N, B_HALF, REC_B_SMOOTH, B_DECAY);
  g.fillRect(0, WAVE_TOP, CONTENT_W, WAVE_H, COL_BG);
  g.drawFastHLine(0, A_CY, CONTENT_W, recAxisColor());
  drawTrackBars(g);
  g.drawFastHLine(0, B_CY, CONTENT_W, recAxisColor());
  drawLiveWaveVisual(g, recLiveWave, B_CY);
}

static void drawRecChrome(m5gfx::M5GFX &g, uint32_t elapsedMs, bool blink, bool ready = false, bool paused = false, uint32_t deleteHeldMs = 0) {
  if (ready || paused) {
    const char *status = ready ? "READY" : "PAUSE";
    uint16_t statusCol = recDimColor();
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
  drawDseg14Text(g, 4, WAVE_BOT + 2, recTime, recAccentColor());
  drawDeleteProgress(g, deleteHeldMs);
}

// 褰曞埗. 寮€鏈鸿嚜鍔ㄨ繘鍏? 璋冪敤姝ゅ嚱鏁板悗鍒涘缓鏂囦欢骞跺紑濮嬪啓鍏?
int recordingScreen() {
  pauseUploadForMedia();
  g_mediaBusy = true;
  auto &d = M5Cardputer.Display;
  g_afterRecord = R_LIST;
  g_uploadAfterRecord = false;

  // 鍑嗗闃舵浠嶆樉绀?READY, 鐪熸寮€濮嬮噰鏍峰悗鎵嶄寒 REC 绾㈢偣
  memset(waveBars, 0, sizeof(waveBars));
  memset(waveBarCounts, 0, sizeof(waveBarCounts));
  memset(recLiveWave, 0, sizeof(recLiveWave));
  M5Canvas cv(&d);
  cv.setColorDepth(8);
  M5Canvas waveCv(&d);
  waveCv.setColorDepth(8);
  bool useWaveSprite = waveCv.createSprite(CONTENT_W, WAVE_H) != nullptr;
  M5Canvas bottomCv(&d);
  bottomCv.setColorDepth(8);
  bool useBottomSprite = useWaveSprite && bottomCv.createSprite(CONTENT_W, CHROME_H) != nullptr;
  bool useSprite = !useWaveSprite && cv.createSprite(CONTENT_W, d.height()) != nullptr;
  const uint32_t recFrameMs = (useWaveSprite || useSprite) ? REC_UI_FRAME_MS : DIRECT_UI_FRAME_MS;
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
      uint16_t statusCol = (ready || pausedFrame) ? recDimColor() : recAccentColor();
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
        drawDseg14Text(d, 4, WAVE_BOT + 2, recTime, recAccentColor());
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
  auto drawRecToast = [&](const char *msg, uint16_t col = recAccentColor()) {
    if (useSprite) drawCanvasToast(cv, msg, col);
    else drawActionToast(msg, col);
  };
  auto drawRecVolume = [&](uint16_t col = recAccentColor()) {
    if (useSprite) drawCanvasVolumeToast(cv, col);
    else drawVolumeToast(col);
  };
  auto drawRecBrightness = [&](uint16_t col = recAccentColor()) {
    if (useSprite) drawCanvasBrightnessToast(cv, col);
    else drawBrightnessToast(col);
  };
  auto drawRecSave = [&]() {
    if (useSprite) drawCanvasSaveBadge(cv);
    else drawActionToast("SAVE", recAccentColor());
  };
  auto cleanupRecSprite = [&]() {
    if (useBottomSprite) bottomCv.deleteSprite();
    if (useWaveSprite) waveCv.deleteSprite();
    if (useSprite) cv.deleteSprite();
    g_mediaBusy = false;
  };
  drawRecFrame(0, false, nullptr, true);

  if (!sdMount()) { cleanupRecSprite(); showMsg("REC", "NO SD", COL_RED); return 0; }
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  int idx = nextRecordingIndex();
  uploadForgetRecordMounted(idx);
  char path[40];
  recordingPathKind(idx, REC_NORMAL, path, sizeof(path));
  File f = SD.open(path, FILE_WRITE);
  if (!f) { cleanupRecSprite(); SD.end(); showMsg("REC", "WRITE ERR", COL_RED); return 0; }
  nextRecHint = (idx < 9999) ? idx + 1 : 9999;
  writeWavHeader(f, REC_RATE, 0);

  bool mustRearm = forceMicRearm;
  bool micWasReady = micInputReady && !mustRearm;
  if (!prepareMicInput(mustRearm)) { cleanupRecSprite(); f.close(); SD.end(); showMsg("REC", "MIC ERR", COL_RED); return 0; }
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
  uint32_t skipBuffers = micWasReady ? 4 : 8;   // 鍑嗗闃舵宸茬儹鏈? 鍙煭鎺愬ご闃叉寜閿０

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
          drawRecToast(cancelled ? "ABORT" : "NO WT", cancelled ? recAccentColor() : recDimColor());
          waitRelease();
        }
        else if (keyUpload()) { drawRecToast("WT", recAccentColor()); g_uploadAfterRecord = true; g_afterRecord = R_LIST; stop = true; break; }
        else if (keyEnter()) { drawRecSave(); g_afterRecord = R_LIST; stop = true; break; }
        else if (keyAlt()) { drawRecToast("WAIT", recAccentColor()); g_afterRecord = R_NOISE; stop = true; break; }
        else if (keyFn() && (keyBrightUp() || keyBrightDn())) { cycleRecAccent(keyBrightUp() ? 1 : -1); saveRecAccentState(); drawRecToast("COLOR", recAccentColor()); waitRelease(); }
        else if (keyVolUp()) { adjustPlayVolume(25); drawRecVolume(recAccentColor()); waitRelease(); }
        else if (keyVolDn()) { adjustPlayVolume(-25); drawRecVolume(recAccentColor()); waitRelease(); }
        else if (keyBrightUp()) { adjustBrightness(1); drawRecBrightness(recAccentColor()); waitRelease(); }
        else if (keyBrightDn()) { adjustBrightness(-1); drawRecBrightness(recAccentColor()); waitRelease(); }
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

// ---------- 褰曢煶鍒楄〃: ;/.閫? 鍥炶溅鏀? Tab+閿粦瀹? Alt闄嶅櫔, 绌烘牸褰曢煶, Esc閫€鍑? 闀挎寜Del鍒犻櫎 ----------
// 杩斿洖 R_BACK(閫€鍑哄垪琛ㄥ苟鎭睆) 鎴?R_RECORD(鍘诲綍闊?
static void deleteRecording(int recNum) {
  if (!sdMount()) return;
  char p[128];
  recordingPathKind(recNum, REC_NORMAL, p, sizeof(p));
  SD.remove(p);
  recordingPathKind(recNum, REC_SHORTCUT, p, sizeof(p));
  SD.remove(p);
  recordingPathKind(recNum, REC_IMPORTANT, p, sizeof(p));
  SD.remove(p);
  recordingPathKind(recNum, REC_MUSIC, p, sizeof(p));
  SD.remove(p);
  uploadForgetRecordMounted(recNum);
  if (recVolumeDirty) saveRecVolumeStateMounted();
  removeRecordingOrderMounted(recNum);
  SD.end();
  setShortcutRec(recNum, false);
  setImportantRec(recNum, false);
  setMusicRec(recNum, false);
  setFrictionDone(recNum, false);
  setFrictionPending(recNum, false);
  setUploadQueued(recNum, false);
  setUploadDone(recNum, false);
  if (recNum > 0 && recNum <= MAX_REC) recDurationSec[recNum - 1] = 0;
  bool hotkeyRemoved = false;
  for (uint8_t group = 0; group < HOTKEY_GROUPS; group++) {
    for (int i = 0; i < hotkeyCount[group]; i++) {
      if (hotkeys[group][i].idx == recNum) {
        for (int j = i; j < hotkeyCount[group] - 1; j++) hotkeys[group][j] = hotkeys[group][j + 1];
        hotkeyCount[group]--; i--;
        hotkeyRemoved = true;
      }
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
  drawHeader("娓呯悊褰曢煶");
  FONT_CN_16(d);
  d.setTextColor(COL_RED, COL_BG);
  d.setCursor(12, 38);
  d.print("鍒犻櫎鎵€鏈夋櫘閫氬綍闊?");
  FONT_CN_12(d);
  d.setTextColor(recDimColor(), COL_BG);
  d.setCursor(12, 66);
  d.print("淇濈暀 QCK / IMP");
  drawFooter("Enter OK  Esc Cancel");
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
  drawHeader("闄嶅櫔纭");
  FONT_CN_16(d);
  d.setTextColor(recAccentColor(), COL_BG);
  d.setCursor(18, 42);
  d.printf("闄嶅櫔 REC_%04d ?", recNum);
  FONT_CN_12(d);
  d.setTextColor(recDimColor(), COL_BG);
  d.setCursor(18, 66);
  d.print("浼氳鐩栧師鏂囦欢");
  drawFooter("Enter OK  Esc Cancel");
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
  if (mode == REC_MUSIC) return isMusicRec(recNum);
  return !isShortcutRec(recNum) && !isImportantRec(recNum) && !isMusicRec(recNum);
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
  if (mode >= REC_KIND_COUNT || sel < 0 || sel >= recCount) return;
  int recNum = recList[sel];
  if (recVisibleInMode(recNum, mode)) g_listModeSelectedRec[mode] = recNum;
}

static int rememberedListIndex(uint8_t mode) {
  if (mode >= REC_KIND_COUNT) return -1;
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

  // 绌哄垪琛?
  if (recCount == 0) {
    d.fillScreen(COL_BG);
    drawStatusTabs(d, g_listMode, 0);
    FONT_ASCII(d);
    d.setTextColor(recDimColor(), COL_BG);
    d.setCursor(28, 58);
    d.print("EMPTY");
    drawDseg14Text(d, 4, 118, shortcutGroupLabel(), recAccentColor());
    if (g_scaleMode) drawDseg14Text(d, 34, 118, "G0", recAccentColor());
    drawDseg14Text(d, CONTENT_W - dseg14TextWidth("0") - 4, 118, "0", recAccentColor());
    waitRelease();
    uint32_t lastInputMs = millis();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        lastInputMs = millis();
        if (keyRollback()) return R_ROLLBACK;
        if (keyFn() && keyChat()) { inputRecAccentPreset(); waitRelease(); continue; }
        if (keyFn() && (keyBrightUp() || keyBrightDn())) { cycleRecAccent(keyBrightUp() ? 1 : -1); saveRecAccentState(); drawActionToast("COLOR", recAccentColor()); waitRelease(); continue; }
        if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(recAccentColor()); waitRelease(); continue; }
        if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(recAccentColor()); waitRelease(); continue; }
        if (keyBrightUp()) { adjustBrightness(1); drawBrightnessToast(recAccentColor()); waitRelease(); continue; }
        if (keyBrightDn()) { adjustBrightness(-1); drawBrightnessToast(recAccentColor()); waitRelease(); continue; }
        if (keySpace()) return R_RECORD;
        if (keyEsc())   return R_BACK;
        if (keyDel())   return R_BACK;
      }
      systemIdleTick();
      if (autoSleepDue(lastInputMs)) return R_BACK;
      delay(8);
    }
  }

  int sel = recCount - 1;   // 榛樿閫夋渶鏂板綍闊?鍒楄〃鏈熬)
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
  int visRows = (d.height() - top - 18) / rowH;   // 搴曢儴鐣?18px 缁欐彁绀?
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
      // 閲嶇粯鍏ㄥ睆鍒楄〃鍐呭
      d.fillRect(0, 0, CONTENT_W, 135, COL_BG);

      // 鏍囬琛?
      drawStatusTabs(d, g_listMode, visibleTotal);

      // 鍒楄〃琛?
      if (visibleTotal == 0) {
        d.setTextColor(recDimColor(), COL_BG);
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
        uint16_t bg = on ? recSelectBgColor() : COL_BG;
        if (on) d.fillRect(0, y - 2, CONTENT_W, rowH - 1, bg);
        // 閫変腑娓告爣
        if (on) { d.setTextColor(recAccentColor(), bg); d.setCursor(2, y); d.print(">"); }
        d.setTextColor(on ? recAccentColor() : recDimColor(), bg);
        char recName[40];
        if (isMusicRec(recList[i])) {
          musicDisplayName(recList[i], recName, sizeof(recName));
          FONT_CN_12(d);
          d.setTextColor(on ? recAccentColor() : recDimColor(), bg);
          d.setCursor(14, y + 2);
          d.print(recName);
        } else {
          snprintf(recName, sizeof(recName), "%04d", recList[i]);
          drawDseg14Text(d, 14, y, recName, on ? recAccentColor() : recDimColor());
        }
        char hk = hotkeyOf(recList[i]);
        bool imp = isImportantRec(recList[i]);
        if (hk) {
          char keyLabel[8];
          snprintf(keyLabel, sizeof(keyLabel), "K:%c", dispKey(hk));
          drawDseg14Text(d, tagX, y, keyLabel, on ? recAccentColor() : recDimColor());
        } else if (imp) {
          drawDseg14Text(d, tagX, y, "IMP", on ? recAccentColor() : recDimColor());
        }
        if (uploadDone(recList[i])) {
          drawDseg14Text(d, uploadX, y, "OKK", on ? recAccentColor() : recDimColor());
        } else if (g_uploadActiveRec == recList[i]) {
          drawDseg14Text(d, uploadX, y, "GO", on ? recAccentColor() : recDimColor());
        } else if (uploadPending(recList[i]) || uploadModelErr(recList[i]) || uploadJobErr(recList[i])) {
          drawDseg14Text(d, uploadX, y, "OKK", on ? recAccentColor() : recDimColor());
        } else if (uploadQueued(recList[i])) {
          drawDseg14Text(d, uploadX, y, "WT", on ? recAccentColor() : recDimColor());
        }
        char dur[8];
        formatDuration(getRecDuration(recList[i]), dur, sizeof(dur));
        d.setTextColor(on ? recAccentColor() : recDimColor(), bg);
        drawDseg14Text(d, durX, y, dur, on ? recAccentColor() : recDimColor());
        if (frictionPending(recList[i])) d.drawFastHLine(CONTENT_W - 16, y + 15, 6, on ? recAccentColor() : recDimColor());
      }
      // 鏇村椤圭澶?
      FONT_CN_12(d); d.setTextColor(recDimColor(), COL_BG);
      if (firstOrdinal > 0)                    { d.setCursor(CONTENT_W - 10, top); d.print("^"); }
      if (firstOrdinal + visRows < visibleTotal) { d.setCursor(CONTENT_W - 10, top + (visRows - 1) * rowH); d.print("v"); }
      // 搴曢儴快捷组
      drawDseg14Text(d, 4, 118, shortcutGroupLabel(), recAccentColor());
      if (g_scaleMode) drawDseg14Text(d, 34, 118, "G0", recAccentColor());
      char cnt[8];
      snprintf(cnt, sizeof(cnt), "%d", visibleTotal);
      drawDseg14Text(d, CONTENT_W - dseg14TextWidth(cnt) - 4, 118, cnt, recAccentColor());
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
    if (keyDel() && !keyTab() && !keyFn() && sel >= 0) {
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
      if (keyRollback()) { waitRelease(); return R_ROLLBACK; }
      if (keyFn() && keyChat()) {
        inputRecAccentPreset();
        redraw = true;
        waitRelease();
        continue;
      }
      if (handleScaleModeChord()) {
        drawActionToast(g_scaleMode ? "SCALE" : "NORM", g_scaleMode ? recAccentColor() : recDimColor());
        redraw = true;
        waitRelease();
        continue;
      }
      if (handleShortcutGroupChord()) {
        drawActionToast(shortcutGroupLabel(), recAccentColor());
        redraw = true;
        waitRelease();
        continue;
      }
      if (handleShortcutKeyRootChord()) {
        drawActionToast(shortcutKeyRootLabel(), recAccentColor());
        redraw = true;
        waitRelease();
        continue;
      }
      if (scaleModeCanHandleKey()) {
        int8_t noteSemis = 0;
        if (scaleNoteSemis(noteSemis)) {
          int recNum = g_scaleCurrentRec;
          if (recNum <= 0 && sel >= 0) recNum = recList[sel];
          if (recNum > 0) {
            g_scaleCurrentRec = recNum;
            if (prepareScaleNoteForRec(recNum, noteSemis)) {
              rememberListSelection(g_listMode, sel);
              g_perfHudPendingKey = pressedBindKey();
              g_nextPlay = recNum;
              return R_PLAY;
            }
          }
        }
        char scaleKey = pressedBindKey();
        if (isScaleSampleKey(scaleKey)) {
          int recNum = findHotkey(scaleKey);
          if (recNum > 0) {
            g_scaleCurrentRec = recNum;
            clearShortcutSemisOverride();
            selectHotkeyPreviewRec(recNum);
            g_perfHudPendingKey = scaleKey;
            g_nextPlay = recNum;
            return R_PLAY;
          } else {
            drawActionToast("NO SMP", recDimColor());
          }
          redraw = true;
          waitRelease();
          continue;
        }
      }
      int hk = pressedHotkeyRec();
      if (hk > 0) {                                     // 缁戝畾閿?鏈€楂樹紭鍏堢骇, 浜ょ粰澶栧眰鎾斁椤?
        rememberListSelection(g_listMode, sel);
        g_listMode = REC_SHORTCUT;
        g_listModeSelectedRec[REC_SHORTCUT] = hk;
        g_perfHudPendingKey = pressedBindKey();
        g_nextPlay = hk;
        return R_PLAY;
      }
      else if (keyFn() && (keyBrightUp() || keyBrightDn())) { cycleRecAccent(keyBrightUp() ? 1 : -1); saveRecAccentState(); drawActionToast("COLOR", recAccentColor()); redraw = true; waitRelease(); }
      else if (keyVolUp()) { adjustPlayVolume(25); drawVolumeToast(recAccentColor()); waitRelease(); }
      else if (keyVolDn()) { adjustPlayVolume(-25); drawVolumeToast(recAccentColor()); waitRelease(); }
      else if (keyBrightUp()) { adjustBrightness(1); drawBrightnessToast(recAccentColor()); waitRelease(); }
      else if (keyBrightDn()) { adjustBrightness(-1); drawBrightnessToast(recAccentColor()); waitRelease(); }
      else if (keyFn() && keyDel()) {
        if (confirmDeleteUnmarked()) {
          int deleted = deleteUnmarkedRecordings();
          sel = recCount - 1;
          g_listMode = REC_NORMAL;
          rememberListSelection(g_listMode, sel);
        }
        redraw = true; waitRelease();
      }
      else if (keyFn() && sel >= 0 && !keyUpload()) {
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
          uint8_t trimTier = 1;
          bool ok = markRecordingShortcut(recList[sel], &trimTier);
          if (ok) setHotkey(bk, recList[sel]);
          if (ok) {
            g_listMode = REC_SHORTCUT;
            rememberListSelection(g_listMode, sel);
            char msg[8];
            snprintf(msg, sizeof(msg), "TRIM%d", trimTier);
            drawActionToast(msg, recAccentColor());
          }
          redraw = true; waitRelease();
        }
      }
      else if (keyFn() && keyUpload()) {
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
        drawActionToast(cancelled ? "ABORT" : (status == UPSTAT_NO_SD ? uploadStatusLabel(status) : "NO WT"), cancelled ? recAccentColor() : recDimColor());
        redraw = true;
        if (M5Cardputer.Keyboard.isPressed()) waitRelease();
      }
      else if (keyUpload() && sel >= 0) {
        if (uploadModeHoldTriggered()) {
          drawActionToast("GO", recAccentColor());
          waitRelease();
          runUploadBatchMounted(true);
          if (g_uploadExitRequested) {
            uint8_t app = g_uploadExitApp;
            g_uploadExitRequested = false;
            g_uploadExitApp = APP_SLEEP;
            if (app == APP_REC_RECORD) return R_RECORD;
            if (app == APP_SLEEP) return R_BACK;
          }
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
          drawActionToast(uploadStatusLabel(status), (status == UPSTAT_QUEUED || status == UPSTAT_DONE) ? recAccentColor() : COL_RED);
        }
        redraw = true;
        if (M5Cardputer.Keyboard.isPressed()) waitRelease();
      }
      else if (keySpace()) { return R_RECORD; }         // 绌烘牸=鍘诲綍闊?
      else if (keyEsc())   { return R_BACK; }           // Esc=閫€鍑哄垪琛ㄥ苟鎭睆
      else if (keyEsc())   { return R_BACK; }
      else if (keyLeft())  { rememberListSelection(g_listMode, sel); g_listMode = (g_listMode + REC_KIND_COUNT - 1) % REC_KIND_COUNT; sel = selectIndexForMode(g_listMode, sel); redraw = true; waitRelease(); }
      else if (keyRight()) { rememberListSelection(g_listMode, sel); g_listMode = (g_listMode + 1) % REC_KIND_COUNT; sel = selectIndexForMode(g_listMode, sel); redraw = true; waitRelease(); }
      else if (keyUp())    {
        if (sel >= 0) {
          int prev = prevVisibleIndex(sel, g_listMode);
          if (prev == sel && g_listMode == REC_NORMAL && loadOlderNormalRecordings()) {
            prev = prevVisibleIndex(sel, g_listMode);
          }
          sel = prev;
          rememberListSelection(g_listMode, sel);
        }
        redraw = true;
      }
      else if (keyDown())  { if (sel >= 0) { sel = nextVisibleIndex(sel, g_listMode); rememberListSelection(g_listMode, sel); } redraw = true; }
      else if (keyEnter() && sel >= 0) {
        rememberListSelection(g_listMode, sel);
        g_nextPlay = recList[sel];
        return R_PLAY;
      }
      else if (keyAlt() && sel >= 0) {
        int recNum = recList[sel];
        if (confirmNoiseReduce(recNum)) {
          char p[128]; recordingPathForRec(recNum, p, sizeof(p));
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

// 鍒楄〃娴佺▼: 鍒楄〃 <-> 褰曢煶 寰幆; Esc閫€鍑哄埌鎭睆
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
      if (pr == R_ROLLBACK) {
        int rolled = rollbackRecordScreen();
        if (rolled > 0) {
          selectRecordedInNormalList(rolled);
          sel = rolled;
        }
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
    if (r == R_ROLLBACK) {
      int rolled = rollbackRecordScreen();
      if (rolled > 0) {
        selectRecordedInNormalList(rolled);
        sel = rolled;
      }
      continue;
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
    showMsg("Wi-Fi", "NO SD", COL_RED);
    return APP_LAUNCHER;
  }
  loadUploadConfig();  // 鍗充娇 url/token 涓嶅畬鏁达紝涔熶細灏介噺璇诲叆宸叉湁瀛楁銆?
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
      if (keyRollback()) {
        waitRelease();
        cleanupWifiSprite();
        return APP_ROLLBACK;
      }
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
    {"SPC", "褰曢煶", APP_REC_RECORD},
    {"ENT", "鍒楄〃", APP_REC_LIST},
    {"C",   "CHAT", APP_CHAT},
    {"F",   "TIMER", APP_POMODORO},
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
      d.print("TOOLS");
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
      drawFooter(";/.閫夋嫨  鍥炶溅杩涘叆  Esc鎭睆", LAUNCHER_DIM);
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyRollback()) { waitRelease(); return APP_ROLLBACK; }
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
  d.print(status && status[0] ? status : "ENTER TALK");
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
    d.print("ASR ONLY");
    d.setCursor(8, 94);
    d.print("涓嶇瓑寰?DeepSeek");
  }
  drawFooter("鍥炶溅璇磋瘽  Space褰曢煶  Esc杩斿洖", dim);
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
  drawFooter("鍥炶溅鍙戦€? Esc鍙栨秷", chatDimColor());

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
      d.print("ENTER SEND");
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
  WavInfo wav;
  if (!parseWavInfo(f, wav) || !f.seek(wav.dataOffset)) {
    f.close();
    return false;
  }
  speakerOn();
  static int16_t pcm[PB_N];
  uint32_t played = 0;
  uint32_t remaining = wav.dataBytes;
  while (remaining > 0) {
    size_t want = remaining > sizeof(pcm) ? sizeof(pcm) : remaining;
    want -= want % wav.blockAlign;
    if (!want) break;
    if (!f.seek(wav.dataOffset + played)) break;
    int got = f.read((uint8_t *)pcm, want);
    if (got == 0) break;
    got -= got % wav.blockAlign;
    if (got <= 0) break;
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
    M5Cardputer.Speaker.playRaw(pcm, samples, wav.sampleRate, wav.channels == 2, 1, 0, false);
    played += got;
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
      if (keyRollback()) {
        chatStopUplink(client);
        chatStopPlaybackTask();
        M5Cardputer.Speaker.stop();
        chatPcmFree();
        client.stop();
        wifiPowerDown();
        waitRelease();
        return APP_ROLLBACK;
      }
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
      drawChatHome("ENTER TALK", reply);
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyRollback()) { waitRelease(); return APP_ROLLBACK; }
      if (keyEsc()) { waitRelease(); return APP_LAUNCHER; }
      if (keySpace()) { waitRelease(); return APP_REC_RECORD; }
      if (keyEnter()) {
        if (!sdMount()) {
          showMsg("CHAT", "NO SD", COL_RED);
          waitRelease();
          redraw = true;
          continue;
        }
        bool ok = chatRecordOnceMounted(CHAT_LAST_PATH);
        SD.end();
        if (!ok) {
          showMsg("CHAT", "REC ERR", COL_RED);
          waitRelease();
          redraw = true;
          continue;
        }
        drawChatHome("姝ｅ湪涓婁紶骞惰瘑鍒?..", nullptr);
        if (!sdMount()) {
          showMsg("CHAT", "SD 璇诲彇澶辫触", COL_RED);
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
          snprintf(reply, sizeof(reply), "OK, NO TEXT");
        } else if (audioUrl[0] && sdMount()) {
          drawChatHome(reply, "姝ｅ湪鎾斁璇煶...");
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
      drawHeader("TIMER");
      FONT_TIMER(d);
      d.setTextColor(remainSec == 0 ? COL_RED : COL_GREEN, COL_BG);
      d.setCursor(40, 52);
      d.print(buf);
      FONT_CN_12(d);
      d.setTextColor(running ? COL_GREEN : COL_DIM, COL_BG);
      d.setCursor(76, 88);
      d.print(running ? "RUN" : (remainSec == 0 ? "DONE" : "PAUSE"));
      drawFooter("鍥炶溅寮€濮?鏆傚仠  Del閲嶇疆  Esc杩斿洖");
      redraw = false;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      lastInputMs = millis();
      if (keyRollback()) { waitRelease(); return APP_ROLLBACK; }
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

static void drawRollbackScreen(const char *status, uint32_t sec = 0, int recNum = 0) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(COL_BG);
  drawStatusTitle(d, "ROLL", COL_GREEN);

  const int bigScale = 2;
  int bigW = dseg14TextWidthScaled(status, bigScale);
  int bigX = (CONTENT_W - bigW) / 2;
  if (bigX < 0) bigX = 0;
  drawDseg14TextScaled(d, bigX, 34, status, COL_GREEN, bigScale);

  char t[16];
  snprintf(t, sizeof(t), "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
  int timeX = (CONTENT_W - dseg14TextWidth(t)) / 2;
  drawDseg14Text(d, timeX, 78, t, COL_DIM);

  if (recNum > 0) {
    char name[16];
    snprintf(name, sizeof(name), "REC_%04d", recNum);
    int nameX = (CONTENT_W - dseg14TextWidth(name)) / 2;
    drawDseg14Text(d, nameX, 98, name, COL_GREEN);
  }

  drawDseg14Text(d, 4, 116, "TAB END", COL_DIM);
}

static int rollbackRecordScreen() {
  pauseUploadForMedia();
  g_mediaBusy = true;
  applyBrightness();
  drawRollbackScreen("START", 0);
  delay(650);
  waitRelease();

  if (!sdMount()) {
    g_mediaBusy = false;
    showMsg("ROLLBACK", "NO SD", COL_RED);
    return 0;
  }
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  SD.remove(ROLLBACK_TMP_PATH);
  File ring = SD.open(ROLLBACK_TMP_PATH, FILE_WRITE);
  if (!ring) {
    SD.end();
    g_mediaBusy = false;
    showMsg("ROLLBACK", "WRITE ERR", COL_RED);
    return 0;
  }

  bool mustRearm = forceMicRearm;
  bool micWasReady = micInputReady && !mustRearm;
  if (!prepareMicInput(mustRearm)) {
    ring.close();
    SD.remove(ROLLBACK_TMP_PATH);
    SD.end();
    g_mediaBusy = false;
    showMsg("ROLLBACK", "MIC ERR", COL_RED);
    return 0;
  }
  if (!micWasReady && !mustRearm) delay(20);
  if (mustRearm) {
    for (int i = 0; i < REC_REARM_SETTLE_BUFFERS; i++) {
      M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
      while (M5Cardputer.Mic.isRecording() > 0) delay(1);
    }
    forceMicRearm = false;
  }

  drawRollbackScreen("START", 0);
  delay(350);
  M5Cardputer.Display.setBrightness(0);

  int b = 0;
  uint32_t ringWrite = 0;
  uint32_t capturedBytes = 0;
  uint32_t lastHiddenStatusMs = millis();
  uint32_t skipBuffers = micWasReady ? 4 : 8;
  bool stop = false;
  bool save = false;
  int savedIdx = 0;
  int rollbackGain = max(recGain, 84);
  int32_t recDc = 0, recHum = 0, recLpf = 0, recNoiseRms = 90, recSoftGateQ8 = 256;

  M5Cardputer.Mic.record(recBuf[0], REC_N, REC_RATE);
  M5Cardputer.Mic.record(recBuf[1], REC_N, REC_RATE);

  while (!stop) {
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (keyRollback()) {
          save = true;
          stop = true;
          applyBrightness();
          drawRollbackScreen("END", min(capturedBytes, ROLLBACK_KEEP_BYTES) / (REC_RATE * 2UL));
          waitRelease();
          break;
        }
        stop = true;
        applyBrightness();
        drawRollbackScreen("END", min(capturedBytes, ROLLBACK_KEEP_BYTES) / (REC_RATE * 2UL));
        waitRelease();
        break;
      }
      if (M5Cardputer.Mic.isRecording() < 2) break;
      delay(1);
    }
    if (stop) break;

    int16_t *filled = recBuf[b];
    processMicBuffer(filled, REC_N, recDc, recHum, recLpf, recNoiseRms, recSoftGateQ8, rollbackGain);
    if (skipBuffers > 0) {
      skipBuffers--;
    } else {
      const uint32_t bytes = REC_N * sizeof(int16_t);
      ring.seek(ringWrite);
      ring.write((uint8_t *)filled, bytes);
      ringWrite += bytes;
      if (ringWrite >= ROLLBACK_KEEP_BYTES) ringWrite = 0;
      if (capturedBytes <= UINT32_MAX - bytes) capturedBytes += bytes;
      if (millis() - lastHiddenStatusMs > 2000) {
        lastHiddenStatusMs = millis();
        ring.flush();
      }
    }
    M5Cardputer.Mic.record(filled, REC_N, REC_RATE);
    b ^= 1;
  }

  while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  micInputReady = false;
  forceMicRearm = true;
  M5Cardputer.Mic.end();
  const uint8_t ES = 0x18;
  M5Cardputer.In_I2C.writeRegister8(ES, 0x0D, 0x01, 400000);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x00, 0x80, 400000);

  ring.flush();
  ring.close();

  uint32_t dataBytes = min(capturedBytes, ROLLBACK_KEEP_BYTES);
  dataBytes &= ~1UL;
  if (save && dataBytes >= REC_RATE) {
    int idx = nextRecordingIndex();
    uploadForgetRecordMounted(idx);
    char path[40];
    recordingPathKind(idx, REC_NORMAL, path, sizeof(path));
    File in = SD.open(ROLLBACK_TMP_PATH, FILE_READ);
    File out = SD.open(path, FILE_WRITE);
    if (in && out) {
      writeWavHeader(out, REC_RATE, 0);
      uint32_t start = (capturedBytes > ROLLBACK_KEEP_BYTES) ? ringWrite : 0;
      uint32_t remaining = dataBytes;
      static uint8_t copyBuf[512];
      while (remaining > 0) {
        uint32_t chunk = min((uint32_t)sizeof(copyBuf), remaining);
        uint32_t untilWrap = ROLLBACK_KEEP_BYTES - start;
        if (chunk > untilWrap) chunk = untilWrap;
        in.seek(start);
        int got = in.read(copyBuf, chunk);
        if (got <= 0) break;
        out.write(copyBuf, got);
        remaining -= got;
        start += got;
        if (start >= ROLLBACK_KEEP_BYTES) start = 0;
      }
      uint32_t written = dataBytes - remaining;
      written &= ~1UL;
      writeWavHeader(out, REC_RATE, written);
      out.flush();
      if (written >= REC_RATE) {
        savedIdx = idx;
        setRecDuration(idx, 44 + written);
        appendRecordingOrderMounted(idx);
        boxSaveRecordedAtMounted(idx);
        insertRecListAtEnd(idx);
        nextRecHint = (idx < 9999) ? idx + 1 : 9999;
        writeNextIndexCache(nextRecHint);
      }
    }
    if (in) in.close();
    if (out) out.close();
    if (savedIdx <= 0) SD.remove(path);
  }

  SD.remove(ROLLBACK_TMP_PATH);
  SD.end();
  g_mediaBusy = false;

  if (save) {
    if (savedIdx > 0) drawRollbackScreen("END", dataBytes / (REC_RATE * 2UL), savedIdx);
    else drawRollbackScreen("END", 0);
    delay(900);
  } else {
    delay(450);
  }
  return savedIdx;
}

static void runApp(uint8_t app) {
  while (true) {
    if (app == APP_SLEEP) return;
    if (app == APP_ROLLBACK) {
      int n = rollbackRecordScreen();
      if (n > 0) listFlow(n);
      return;
    }
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

// ---------- 楹﹀厠椋庨鐑?寮€鏈?/ 鍞ら啋鍚庤皟鐢?: 绌鸿窇娑堣€楀喎鍚姩涓嶇ǔ瀹? 涔嬪悗淇濇寔甯稿紑 ----------
static void micWarmup(bool rearmAfterWarmup = true, int warmBuffers = 24) {
  // 寮€鏈?鍞ら啋鐨勭涓€鏉″綍闊充篃蹇呴』璧板畬鏁磋緭鍏ラ摼璺厤缃? 鍚﹀垯棣栨褰曢煶澧炵泭浼氬亸浣庛€?
  micInputReady = false;
  if (!prepareMicInput()) return;
  forceMicRearm = rearmAfterWarmup;
  static int16_t warm[REC_N];
  for (int i = 0; i < warmBuffers; i++) {
    M5Cardputer.Mic.record(warm, REC_N, REC_RATE);
    while (M5Cardputer.Mic.isRecording() > 0) delay(1);
  }
  // 涓?Mic.end(): 淇濇寔甯稿紑
}

// ---------- 鎭睆(杞荤潯鐪?: 鍏宠儗鍏? 涓€鐩寸潯鍒?閿洏涓柇/Go閿?鎵嶉啋; 绌烘牸=褰曢煶, 鍥炶溅=鍒楄〃, C=CHAT, F=鐣寗閽? Go=搴旂敤鍒楄〃 ----------
// 鏍稿績鎬濊矾: 涓嶈皟 Mic.end() 鈫?ES8311 妯℃嫙娈典繚鎸侀€氱數, 娌℃湁鎺夌數鐬€? 娌℃湁鐖嗛煶.
// CONFIG_PM_ENABLE 鏈惎鐢? I2S 涓嶆寔鏈夐樆姝㈣交鐫＄湢鐨勭數婧愰攣, 鍙洿鎺?esp_light_sleep_start().
// I2S 浠诲姟鍦ㄧ潯鐪犳湡闂磋 RTOS 鏆傚仠, APB 鏃堕挓闂ㄦ帶; 鍞ら啋鍚庤嚜鍔ㄧ画璺?
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

  // 闈欓煶 ES8311 杈撳嚭璺緞(涓嶆帀鐢?0x0D/0x12): 闃叉鐫＄湢鐬€佽鏃?mute 鑴氬姛鏀炬斁澶?
  const uint8_t ES = 0x18;
  if (speakerOutputReady) M5Cardputer.Speaker.setVolume(0);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x32, 0x00, 400000);  // 鍏堟柇寮€ DAC->HP mixer
  delay(2);
  M5Cardputer.In_I2C.writeRegister8(ES, 0x13, 0x00, 400000);  // 鍏?HP drive
  delay(5);
  M5Cardputer.Speaker.end();
  speakerOutputReady = false;
  speakerColdStarted = false;

  bool uploadBeforeSleep = false;
#if UPLOAD_WIFI_ENABLED
  if (!g_mediaBusy && sdMount()) {
    uploadBeforeSleep = uploadQueueHasJobMounted();
    SD.end();
  }
#endif
  d.fillScreen(COL_BG);
  if (uploadBeforeSleep) applyMinBrightness();
  else d.setBrightness(0);          // 鍏宠儗鍏?= 鏈€澶х渷鐢电偣
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
  if (uploadBeforeSleep) runUploadBatchMounted(true);
  else if (!g_mediaBusy) runUploadBatchMounted(false);
  if (g_uploadExitRequested) {
    uint8_t app = g_uploadExitApp;
    g_uploadExitRequested = false;
    g_uploadExitApp = APP_SLEEP;
    if (app != APP_SLEEP) {
      applyBrightness();
      M5Cardputer.update();
      wakeApp = app;
      autoRecordPending = true;
      waitRelease();
      return;
    }
    d.setBrightness(0);
  } else if (g_uploadModeVisible) {
    d.setBrightness(0);
    g_uploadModeVisible = false;
  }
#endif

  // 鎵撳紑閿洏鑺墖(TCA8418 @0x34)鐨勬寜閿腑鏂?-> 鎸夐敭鏃舵媺浣?GPIO11; Go/BtnA 鏄?GPIO0 浣庢湁鏁?
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x01, 400000);   // CFG: KE_IEN=1
  gpio_wakeup_enable(GPIO_NUM_11, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  while (true) {
    M5Cardputer.update();                                          // 鎺掔┖閿洏浜嬩欢
    M5Cardputer.In_I2C.writeRegister8(0x34, 0x02, 0x03, 400000);  // 娓呬腑鏂爣蹇?-> INT 绾垮浣嶄负楂?
    esp_light_sleep_start();                                       // 涓€鐩寸潯鍒?GPIO11 鎴?GPIO0 鍙樹綆
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

  // 鍞ら啋: 鍏虫帀鍞ら啋婧?+ 閿洏涓柇
  gpio_wakeup_disable(GPIO_NUM_11);
  gpio_wakeup_disable(GPIO_NUM_0);
  M5Cardputer.In_I2C.writeRegister8(0x34, 0x01, 0x00, 400000);
  applyBrightness();
  applyBrightness();
  if (wakeApp == APP_REC_RECORD) micWarmup(false, 16);  // 鎭睆鍞ら啋澶嶇敤棰勭儹閾捐矾, 鍑忓皯寮€濮嬬垎闊冲拰绛夊緟
  autoRecordPending = true;
  waitRelease();
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // 寮€鏈轰笉鑷姩寮€鎵０鍣?娑堥櫎寮€鏈哄姛鏀句笂鐢?鐖嗛煶"), 鎾斁鏃跺啀鎵嬪姩寮€
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(SCREEN_ROT);
  applyBrightness();
  M5Cardputer.Display.setTextWrap(false);   // 鍏抽棴鑷姩鎹㈣: 杩囬暱鏂囧瓧鍙充晶鎴柇, 涓嶄細鎹㈣鎺夊嚭灞忓箷
#if UPLOAD_WIFI_ENABLED
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
#endif
  {                          // internal_spk=false 璺宠繃鎵０鍣ㄩ厤缃? 鎵嬪姩琛ヤ笂 Adv 鎵０鍣ㄥ紩鑴?
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
