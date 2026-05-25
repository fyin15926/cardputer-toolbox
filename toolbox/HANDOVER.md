# Cardputer 录音工具箱 — 项目交接文档

## 一、硬件

| 项目 | 说明 |
|------|------|
| 设备 | M5Stack Cardputer-Adv |
| 主控 | ESP32-S3，240 MHz，320KB RAM，1310720B Flash |
| 屏幕 | 240×135 ST7789V2，横屏（SCREEN_ROT=1） |
| 键盘 | TCA8418 I2C 键盘控制器，中断引脚 GPIO11 |
| 音频 | ES8311 编解码器（I2C 地址 0x18）+ NS4150B D 类功放 |
| 麦克风 | I2S_NUM_0，数据 GPIO46，WS GPIO43，BCK GPIO41 |
| 扬声器 | I2S_NUM_1，数据 GPIO42，WS GPIO43，BCK GPIO41（共享 41/43） |
| 电池 | 1750mAh |
| SD 卡 | SPI，SCLK=40，MISO=39，MOSI=14，CS=12 |

**重要硬件限制：NS4150B 功放没有软件可控的 SD/MUTE 引脚。** 无法通过软件完全切断放大器，这是爆音问题的根本硬件约束。

---

## 二、源文件位置

| 角色 | 路径 |
|------|------|
| GitHub 仓库（主编辑） | `D:\github仓库同步\小机器\toolbox\toolbox.ino` |
| Arduino 编译副本 | `C:\cardputer\sketches\toolbox\toolbox.ino` |
| 本文档 | `D:\github仓库同步\小机器\toolbox\HANDOVER.md` |
| 云端转写方案 | `D:\github仓库同步\小机器\toolbox\CLOUD_TRANSCRIBE_FLOMO_PLAN.html` |
| 电脑端 net.txt 生成器 | `D:\github仓库同步\小机器\toolbox\NET_CONFIG_HELPER.html` |

**每次修改 `.ino` 后需要手动同步到编译副本：**
```powershell
Copy-Item "D:\github仓库同步\小机器\toolbox\toolbox.ino" `
          "C:\cardputer\sketches\toolbox\toolbox.ino" -Force
```
（中文路径在某些终端下有问题，编译始终用 `C:\cardputer\sketches\toolbox`）

---

## 三、Arduino CLI 环境

### 二进制
```
C:\cardputer\tools\arduino-cli\arduino-cli.exe   （不在系统 PATH 中）
版本：1.5.0
```

### 配置文件（自动加载，无需 --config-file）
```
C:\Users\87194\AppData\Local\Arduino15\arduino-cli.yaml
```
内容：
```yaml
board_manager:
    additional_urls:
        - https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json
directories:
    user: C:\cardputer\Arduino
network:
    connection_timeout: 1200s
```

### esp32 Core
```
已安装版本：3.1.3-cn
位置：C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.1.3\
FQBN：esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB
```
2026-05-23 踩坑记录：曾出现 `Platform 'esp32:esp32' not found`，原因是
`packages\esp32\hardware\esp32\3.1.3` 目录缺失，但 `packages\esp32\tools` 仍残留。
这种情况无需清理 tools，直接执行 `core install esp32:esp32@3.1.3` 可恢复。

注意：实测可用镜像是：
```
https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json
```
不要使用：
```
https://jihulab.com/esp-mirror/arduino/arduino-esp32/-/raw/gh-pages/package_esp32_cn_index.json
```
这条在 2026-05-23 返回 `401 Unauthorized`。

### 依赖库
位于 `C:\cardputer\Arduino\libraries\`：
- `M5Unified`（master 分支，含 Cardputer-Adv 回调）
- `M5GFX`

---

## 四、编译 & 烧录

```powershell
$cli    = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"
$sketch = "C:\cardputer\sketches\toolbox"
$build  = "C:\cardputer\build_fresh\out"
$fqbn   = "esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB"

# 编译
& $cli compile --fqbn $fqbn --build-path $build $sketch

# 烧录（设备通过 USB CDC 挂载为 COM3；若设备在休眠则先按空格或回车唤醒）
& $cli upload --fqbn $fqbn --port COM3 --input-dir $build
```

2026-05-24 更新：为容纳 Wi-Fi 上传模块，后续编译/烧录统一使用 8MB Flash + 3MB APP 分区：
`esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB`。
最近一次编译通过：程序 1,739,024 bytes（52% / 3,342,336），全局变量 128,624 bytes（39% / 327,680）。
最近一次烧录通过：COM3，写入 1,739,408 bytes，Hash verified，Hard resetting with RTC WDT。

### 2026-05-24 联网上传当前状态

- 固件已烧录进小机器：盒子层 Wi-Fi、轻量 HTTP 上传、上传队列、NTP 校时都在固件内。
- 端到端已真机验证成功：Cardputer 录音后按 `\` 上传，服务器收到 WAV，完成转写并推送到 flomo。
- 录音应用只表达用户意图：按 `\` 上传“当前/选中这一条”，不会自动上传全部录音。
- 盒子层负责联网和上传：读取 `SD:/UPLOAD/net.txt`，连接 Wi-Fi，HTTP POST 到服务器，成功后写 `SD:/UPLOAD/done.txt`。
- 小机器工具箱新增 `Wi-Fi` 应用并已成功烧录：可现场输入/修改 Wi-Fi 名和密码，保存后测试连接，并保留已有服务器 URL/token。
- 电脑负责复杂服务器配置：用 `toolbox/NET_CONFIG_HELPER.html` 生成 `net.txt`，不在小机器小键盘上敲长 token。
- 服务器负责复杂业务：鉴权、保存 WAV、阿里云转写、flomo Webhook；小机器不保存阿里云 Key 或 flomo Webhook。
- 服务器窗口已经完成云端闭环：正式 URL 是 `http://cardputer.flye.cc/upload`；`UPLOAD_TOKEN` 只保存在服务器 `/etc/cardputer-voice.env`，不要写进 GitHub。
- `SD:/UPLOAD/net.txt` 已成功用于真实上传；Wi-Fi 可在小机器 `Wi-Fi` 应用里现场改，服务器 URL/token 继续由电脑配置页维护。
- 录音列表状态含义：`UP` = 已入上传队列/等待或重试上传；`OK` = 设备已收到服务器 2xx，WAV 已上传到服务器。当前 `OK` 不表示设备确认 flomo 完成；flomo 完成由服务器 job 状态判断。
- 下一步建议：服务器侧做可靠性和可观测性（`/jobs` 最近列表、failed 重试、服务重启后扫描未完成 jobs、避免 flomo 重复发送）。设备侧若要更精确，可把列表 `OK` 改为 `SV`，再新增查询 job 状态后显示 flomo 完成。

`SD:/UPLOAD/net.txt` 格式：
```text
ssid=你的WiFi名
password=你的WiFi密码
url=http://服务器地址:3000/upload
token=服务器给的UPLOAD_TOKEN
device=cardputer-001
ntp=pool.ntp.org
tz=8
```

### 2026-05-24 服务器/flomo 当前状态

- 阿里云 ECS：公网 IP `47.110.91.244`，域名 `cardputer.flye.cc` 已解析并通过公网验证。
- 部署目录：`/opt/cardputer-voice`；运行服务：`cardputer-voice.service`；nginx 配置：`/etc/nginx/conf.d/cardputer-voice-domain.conf`。
- 已配置环境变量：`UPLOAD_TOKEN`、`ASR_FILE_TOKEN`、`DASHSCOPE_API_KEY`、`FLOMO_WEBHOOK_URL`、`PUBLIC_BASE_URL=http://cardputer.flye.cc`。这些真实密钥只在服务器上，不进仓库。
- 接口：
  - `GET http://cardputer.flye.cc/health`
  - `POST http://cardputer.flye.cc/upload`
  - `GET http://cardputer.flye.cc/jobs/REC_XXXX`
  - `POST http://cardputer.flye.cc/jobs/REC_XXXX/process`
- DashScope 录音文件识别要求公网可访问音频 URL，所以服务器提供带私密 token 的 `/audio/REC_XXXX.wav` 给 DashScope 拉取，不对普通用户公开。
- 端到端测试已通过：上传 `REC_ASR_0001.wav` 后自动转写为 `12345。上山打老虎。`，写入 `/opt/cardputer-voice/transcripts/REC_ASR_0001.txt`，并成功发送到 flomo，job 状态 `done`。
- GitHub 已同步服务器代码和部署模板：`2c8d05d Add DashScope transcription and flomo sync`。详见 `cloud-voice-server/DEPLOYMENT.md`。

`C:\cardputer\tools\build_and_flash.ps1` 曾因文件编码损坏导致 PowerShell 解析错误
（中文乱码、字符串缺少结束符）。如果脚本报错，优先按上面的手动三步走；
需要一键脚本时，用 `COMPILE_FLASH.md` 第 9 节的 UTF-8 版本覆盖重写。

---

## 五、GitHub 推送

端口 22 被封，必须走 SSH over 443：
```bash
git remote set-url origin ssh://git@ssh.github.com:443/fyin15926/cardputer-toolbox.git
git add toolbox.ino
git commit -m "说明"
git push
```

---

## 六、固件功能说明

### 界面流程
```
开机 / 息屏
 ├─ 开机  → 自动进入真正录音 → 录音结束 → 录音列表（自动定位到刚录的条目）
 ├─ 息屏按空格 → 唤醒并直接录音
 └─ 息屏按回车 → 唤醒并进入录音列表

录音中
 ├─ 空格  → 暂停 / 继续录音（暂停时不写 SD，计时停止）
 ├─ 回车  → 停止并保存 → 录音列表
 ├─ Esc   → 停止并保存 → 息屏
 ├─ 长按 Del 1.2秒 → 进度条走满后取消并删除本条 → 息屏
 └─ W/S   → 软件增益调节

录音列表
 ├─ ;/.   → 上下选择
 ├─ 回车  → 播放
 ├─ 长按 Del 1.2秒 → 进度条走满后删除选中录音
 ├─ Alt   → 降噪确认；回车确认后覆盖原文件，退格取消
 ├─ 左/右 → 切换三列目录：REC / KEY / IMP（普通 / 快捷 / 重要）
 ├─ Ctrl+键 → 给该录音绑定快捷键，并移动到 SD:/SHORTCUT
 ├─ Ctrl+Enter → 标记为重要录音，并移动到 SD:/IMPORTANT
 └─ Esc   → 退出列表并息屏

播放中
 ├─ 播完  → 自动返回录音列表
 ├─ 回车  → 暂停 / 继续播放
 ├─ 退格  → 返回录音列表
 ├─ ;/.   → 切换上一条 / 下一条录音
 ├─ Esc   → 停止播放并息屏
 ├─ +/-   → 音量
 ├─ 空格  → 返回去录音
 └─ 长按 Del 1.2秒 → 进度条走满后删除当前录音 → 返回列表
```

### UI 简化原则（2026-05-23 更新）
- 已移除右侧三段标签栏，所有界面使用 240px 全宽内容区。
- 开机/空格唤醒不再停留 READY 页，`setup()` 后第一次 `loop()` 会直接进入 `recordingScreen()` 开始录音；息屏时按回车会直接进入录音列表。
- 独立启动页和 READY 监听页已清理；`READY` 只作为录音准备帧，真正 `Mic.record()` 启动后才显示 REC 红点。
- 统一双线命名：**A线 / 轨道线 / Timeline A**，**B线 / 监听线 / Monitor B**。
- 播放页也使用同一套双线布局：A线显示播放时间轴和播放头，B线使用当前播放缓冲跳动；播放时不额外开麦克风。
- A线和B线之间不绘制额外分隔线，只保留各自的暗基线，减少视觉噪音。
- 播放页不再进入前完整扫描 WAV 生成 A线；按下播放后先启动声音，A线在扬声器队列空隙里后台逐格预览填充，避免 SD 预读造成约 1 秒等待。
- 播放页改为 Canvas 双缓冲推屏，减少 A/B 线和计时器的直接擦屏频闪。
- A线统一为 60 个亮绿竖向小波形柱：录制页直接每帧推进 1 柱；播放页按 60 个柱快速抽样，每柱只取 1 个 256-sample 中点小片，且只在扬声器队列有余量时小预算推进，避免渲染 A线时抢 SD 造成播放卡顿。
- 播放自然结束后自动返回列表，减少“听完还停在播放页”的多余停留。
- 进入播放页后不再阻塞等待回车松开；启动按键会被非阻塞忽略，音频队列可以立即开始填充。
- 录音中 Space 改为暂停/继续：暂停时停止写入 SD、计时冻结；继续时丢弃恢复瞬间的 2 帧缓冲，避免按键声写入。
- 息屏键统一为 Esc：列表页 Esc 退出息屏，播放页 Esc 停止播放并息屏，录音中 Esc 先保存当前录音再息屏。Cardputer 库里物理 Esc 映射为左上角反引号键。
- 删除操作统一为长按 Del 1.2 秒：录音中删除本条、播放页删除当前录音、列表页删除选中录音；按住时显示红色进度条和“删除中”，松开即取消。
- 录音准备阶段不再等待启动键松开才开始采样；改为立刻启动录音缓冲，并在松开前忽略停止键事件。
- 麦克风开机/唤醒后会热机并强制重建一次输入链路；只在从播放等扬声器状态切回来时重新准备麦克风。
- 开机/唤醒预热必须调用 `prepareMicInput()` 的完整输入链路配置；否则第一次录音会因为寄存器路径不完整而音量偏低。
- 开机/唤醒后的第一次正式录音设置 `forceMicRearm=true`：录音前强制 `Mic.end()` → 完整 `prepareMicInput(true)` → 丢弃 6 帧稳定缓冲，避免第一条录音音量偏低。
- 录音准备阶段保持 READY 显示；只有 `Mic.record()` 真正启动后才显示 REC 红点，避免“还没录就闪红点”。
- 新录音编号使用 `nextRecHint` 缓存，并同时避开 `/REC`、`/SHORTCUT` 与 `/IMPORTANT` 中已有编号，减少录音多时的启动等待。
- 录音开始时查找新编号会顺手填充录音列表缓存；保存后直接把新编号插入缓存，回列表不再重复扫描 SD。
- 录音保存时先显示 `SAVE` 即时反馈，并去掉显式 `flush()`，交给 `close()` 完成收尾，减少回列表前的重复等待。
- 不再使用“回车直接播放最新录音”的隐藏捷径，避免误触和界面跳转分叉。
- 统一交互：**息屏空格录音、息屏回车进列表；录音中空格暂停/继续、回车保存进列表、Esc保存并息屏；列表回车播放选中录音；播放中回车暂停/继续、退格回列表、;/.切换上一条/下一条、Esc息屏**。
- 反斜杠键不再作为主流程按键，列表/录音/播放入口统一交给回车与退格，减少重复分支。
- 所有主要按键先给即时反馈再执行慢操作：列表播放显示 `PLAY`，播放切换显示 `PREV/NEXT`，暂停/继续显示 `PAUSE/PLAY`，返回/录音/息屏也会先闪提示，避免用户误以为没按到。
- 播放流程复用当前录音列表缓存，只有缓存缺失时才重新扫描 SD，减少列表播放和播放中上下切换的等待。
- 列表删除走满进度后先显示 `DEL`，删除后只从内存列表移除该条；不再每次删除后重新扫描 SD，且只有删除了绑定快捷键时才重写 `/SHORTCUT/keys.txt`。
- 快捷键直播放仍保留，适合高频音效/录音触发。
- 列表有三列目录：`REC` 普通、`KEY` 快捷、`IMP` 重要，左右键切换；播放页按上下切换时只在当前列内切换。播放返回列表时会重新选中最后正在播放的录音。
- 重要/快捷目录：绑定快捷键时文件从 `/REC` 移到 `/SHORTCUT`；按 `Ctrl+Enter` 标记重要时文件从 `/REC` 移到 `/IMPORTANT`。这样清空普通 `/REC` 时不会影响快捷和重要录音。
- 列表 `Alt` 降噪已加二次确认，避免误覆盖原文件。

### 方向键映射
| 物理键 | 逻辑 |
|--------|------|
| `;`    | 上   |
| `.`    | 下   |
| `,`    | 左   |
| `/`    | 右   |

### 录音文件
- 路径：普通录音在 `/REC/REC_XXXX.wav`，快捷录音在 `/SHORTCUT/REC_XXXX.wav`，重要录音在 `/IMPORTANT/REC_XXXX.wav`，最多 9999 条
- 格式：16kHz、16-bit、单声道 PCM WAV
- 快捷键映射：`/SHORTCUT/keys.txt`，格式为每行 `键,编号`；旧版 `/REC/keys.txt` 和 `/IMPORTANT/keys.txt` 会在启动时迁移读取并写入新位置

---

## 七、ES8311 音频架构与爆音问题（关键）

### 信号路径
```
麦克风 → ES8311 ADC → I2S_NUM_0 → ESP32（录音）
ESP32 → I2S_NUM_1 → ES8311 DAC → HP 输出 → NS4150B → 扬声器（播放）
```

### 关键寄存器
| 寄存器 | 值 | 含义 |
|--------|-----|------|
| `0x00` | `0x80` | CSM（时钟系统）上电 |
| `0x00` | `0x00` | CSM 断电 |
| `0x0D` | `0x01` | 模拟段（ADC+DAC 偏置）上电 **← 爆音的根源** |
| `0x0D` | `0xFC` | 模拟段断电，产生跳变瞬态，经 NS4150B 放大为爆音 |
| `0x12` | `0x00` | DAC 使能 |
| `0x12` | `0xFC` | DAC 关闭 |
| `0x13` | `0x10` | HP 驱动使能 |
| `0x13` | `0x00` | HP 驱动关闭 |
| `0x32` | `0xBF` | DAC→HP 混音器（播放模式） |
| `0x32` | `0x00` | DAC→HP 混音器断开（录音模式） |

### M5Unified 回调行为
`Mic.end()` 触发 `_microphone_enabled_cb_cardputer_adv(false)`，**固定写入**：
```
0x0D = 0xFC  ← 模拟段断电（爆音！）
0x0E = 0x6A
0x00 = 0x00  ← CSM 断电
```
只要麦克风任务 handle 非空（即 mic 正在运行），调用 `Mic.end()` 就会触发此回调。  
**任务 handle 为 null 时 `Mic.end()` 是空操作，不触发回调，不产生爆音。**

`Speaker.end()` 的回调是空的，不写任何寄存器。

### 各场景爆音状态（当前固件）

| 场景 | 状态 | 说明 |
|------|------|------|
| 开机 | ⚠️ 轻微 | `micWarmup()` → `Mic.begin()` 上电模拟段，NS4150B 放大瞬态；硬件限制，软件难以消除 |
| 进入息屏 | ✅ 已消除 | `goSleep()` 不调 `Mic.end()`，不触发 `0x0D=0xFC`；CONFIG_PM_ENABLE 未设置，I2S 无电源锁，直接可以 light sleep |
| 停止录音 | ⚠️ 可能轻微 | 录音结束时调 `Mic.end()`（必须），回调写 `0x0D=0xFC`；通过 `0x32=0x00` 断路 + 立即写回 `0x0D=0x01`（~300µs 窗口）减轻；如仍有声音是硬件限制 |
| 播放（任何次） | ✅ 已消除 | 录音结束时 `Mic.end()` 已将 task handle 置 null；`speakerOn()` 内的 `Mic.end()` 是空操作，不触发回调 |

### `CONFIG_PM_ENABLE` 说明
```
C:\Users\87194\AppData\Local\Arduino15\packages\esp32\tools\
  esp32-arduino-libs\idf-release_v5.3-489d7a2b-v1\esp32s3\qio_qspi\include\sdkconfig.h
```
此文件中 `CONFIG_PM_ENABLE` **未定义**，因此 I2S 不持有阻止 light sleep 的电源锁，可以直接调用 `esp_light_sleep_start()` 而不需要先停止 I2S。

---

## 八、息屏（轻睡眠）机制

```cpp
// goSleep() 核心流程：
1. 写 0x13=0x00（关 HP drive）、0x12=0xFC（关 DAC）— 静音但不断电模拟段
2. 关背光
3. TCA8418 寄存器 0x01 写 0x01（开键盘中断 KE_IEN）
4. gpio_wakeup_enable(GPIO11, LOW_LEVEL) + esp_sleep_enable_gpio_wakeup()
5. 循环：清中断 → esp_light_sleep_start()（30s 安全唤醒兜底）→ 检测是否空格键
6. 唤醒后：关唤醒源 → 开背光 → micWarmup() → 标记自动录音
```

唤醒条件：只有**空格键**才真正唤醒；其他键按下会唤醒 CPU 但立即再次入睡。

---

## 九、待处理问题

1. **停录轻微爆音**：硬件上 NS4150B 无 SD 引脚，软件已尽力压缩断电窗口。若需彻底消除，需要硬件修改（串联 RC 到 NS4150B 输入，或更换带 MUTE 引脚的功放）。
2. **开机轻微爆音**：同上，`Mic.begin()` 首次上电模拟段不可避免产生瞬态。
3. **一键脚本需重写**：当前本地 `build_and_flash.ps1` 有编码损坏风险，推荐用 `COMPILE_FLASH.md` 里的版本重建。

---

## 十、常用调试

```powershell
# 查看已安装 core
& "C:\cardputer\tools\arduino-cli\arduino-cli.exe" core list

# 查看串口（烧录前确认设备已连接且不在休眠）
& "C:\cardputer\tools\arduino-cli\arduino-cli.exe" board list

# 查看编译/烧录日志
Get-Content "C:\cardputer\tools\tb.out.log"
Get-Content "C:\cardputer\tools\tb.err.log"
Get-Content "C:\cardputer\tools\up.err.log"
```

若设备在休眠（COM3 断开），先按空格或回车唤醒再烧录。
---

## 2026-05-24 audio-pop and startup/list performance notes

Current practical target:
- Sleep idle must never produce surprise pops. This is the highest priority.
- Playback start pop is handled with an 80 ms PCM fade-in.
- Sleep -> Space -> record is now the best path: no obvious start pop and no obvious delay.
- Recording stop still produces a predictable pop from `Mic.end()` / ES8311 analog power transition. Treat this as a hardware/codec limit unless a hardware mute is added.

Confirmed by testing:
- Plugging in headphones stops the speaker pop, but the pop is still audible in the headphones. Therefore the source is before the speaker amp, inside the ES8311/output analog path, not only NS4150B.
- Software cannot truly switch "recording to headphone output, playback to speaker output"; headphone/speaker switching is controlled by the jack detect / AMP_EN hardware path, not an exposed GPIO in the current design.
- ES8311 digital mute / DAC soft-ramp register attempts did not reduce the record start/stop pop.
- A feedback beep before record transitions did not mask the pop; the pop occurred after the beep.
- Keeping the mic hot across too many states caused worse bugs in earlier experiments: first recording low/no volume, second recording silent, or huge second-record pop.

Current firmware strategy:
- `goSleep()` closes mic only at real sleep entry if needed, then disables the output path and does not use periodic timer wake. This keeps sleep idle quiet.
- Sleep wake to record uses `micWarmup(false, 16)` so the prewarmed mic chain is reused instead of being rebuilt. This avoids the sleep-wake record start pop.
- Cold boot uses a conservative mic warmup/rearm path so the first recording has audio.
- Playback uses the 80 ms PCM fade-in.
- Startup loads recordings/hotkeys so list entry does not block.

Performance finding:
- The long delay was not `M5Cardputer.begin()`, `SD.begin()`, or mic warmup. Serial timing showed the big cost was scanning all recording directories to find/list recordings.
- `.next` cache was added under `/REC/.next` so cold boot recording can find the next index without a full scan.
- A bug appeared where old recordings seemed missing because the fast path skipped scanning and the in-memory list only knew newly created recordings. Fix: startup scans recordings; list entry no longer forces a scan every time.

Cleanup feature:
- In the list screen, `Ctrl+Del` opens a confirmation for deleting all unmarked normal recordings.
- It deletes only `/REC/REC_XXXX.wav`.
- It preserves `/SHORTCUT` and `/IMPORTANT` recordings.
- This is the recommended practical way to keep list scanning fast when many ordinary recordings accumulate.

Do not reintroduce without a very specific reason:
- Periodic sleep timer wake (`esp_sleep_enable_timer_wakeup(30000000ULL)`): caused sleep-idle random pops.
- Mic-hot/no-`Mic.end()` across playback/list/sleep without clear boundaries: caused no-audio/low-audio regressions.
- Delaying record-stop `Mic.end()` until playback start: moved the pop to playback start, which was worse.
- ES8311 DAC digital mute / 0x31/0x37 ramp experiment: no improvement for record transition pop.
