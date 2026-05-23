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
FQBN：esp32:esp32:m5stack_cardputer
```
Core 是完整的，无需重新安装。

### 依赖库
位于 `C:\cardputer\Arduino\libraries\`：
- `M5Unified`（master 分支，含 Cardputer-Adv 回调）
- `M5GFX`

---

## 四、编译 & 烧录

```powershell
$cli    = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"
$sketch = "C:\cardputer\sketches\toolbox"
$fqbn   = "esp32:esp32:m5stack_cardputer"

# 编译
& $cli compile --fqbn $fqbn $sketch

# 烧录（设备通过 USB CDC 挂载为 COM3；若设备在休眠则先按空格唤醒）
& $cli upload --fqbn $fqbn --port COM3 $sketch
```

编译通过后资源占用约：程序 86%（1134KB / 1310KB），全局变量 23%（75KB / 327KB）。

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
主屏
 ├─ 空格          → 录音 → 录音结束 → 录音列表（自动定位到刚录的条目）
 ├─ 回车（光标在"录音列表"）→ 录音列表（默认选最新录音）
 ├─ 绑定键（字母/数字）→ 直接播放对应录音
 └─ 退格 / 60s 无操作 → 息屏（轻睡眠）
     └─ 空格唤醒 → 主屏

录音中
 ├─ 空格  → 暂停/继续
 ├─ 回车  → 停止并保存 → 录音列表
 └─ W/S   → 软件增益调节

录音列表
 ├─ ;/.   → 上下选择
 ├─ 回车  → 播放
 ├─ N     → 对选中录音做频域降噪（覆盖原文件）
 ├─ Ctrl+键 → 给该录音绑定快捷键（存到 SD:/REC/keys.txt）
 └─ 退格  → 返回主屏

播放中
 ├─ 回车  → 暂停/继续
 ├─ +/-   → 音量
 ├─ 空格  → 返回去录音
 └─ 退格  → 返回列表
```

### 方向键映射
| 物理键 | 逻辑 |
|--------|------|
| `;`    | 上   |
| `.`    | 下   |
| `,`    | 左   |
| `/`    | 右   |

### 录音文件
- 路径：SD 卡 `/REC/REC_XXXX.wav`，最多 9999 条
- 格式：16kHz、16-bit、单声道 PCM WAV
- 快捷键映射：`/REC/keys.txt`，格式为每行 `键,编号`

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
6. 唤醒后：关唤醒源 → 开背光 → micWarmup() → 重绘主屏
```

唤醒条件：只有**空格键**才真正唤醒；其他键按下会唤醒 CPU 但立即再次入睡。

---

## 九、待处理问题

1. **停录轻微爆音**：硬件上 NS4150B 无 SD 引脚，软件已尽力压缩断电窗口。若需彻底消除，需要硬件修改（串联 RC 到 NS4150B 输入，或更换带 MUTE 引脚的功放）。
2. **开机轻微爆音**：同上，`Mic.begin()` 首次上电模拟段不可避免产生瞬态。

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

若设备在休眠（COM3 断开），先按空格唤醒再烧录。
