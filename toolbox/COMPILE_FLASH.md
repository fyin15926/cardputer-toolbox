# Codex 编译与烧录指南
# Compile & Flash Guide for Codex

> 本文档描述 M5Stack Cardputer-Adv 项目的完整「编辑 → 编译 → 烧录」流程。  
> This document describes the full Edit → Compile → Flash workflow for the M5Stack Cardputer-Adv project.

---

## 目录 / Table of Contents
1. [环境概览](#1-环境概览)
2. [第一步：编辑源码](#2-第一步编辑源码)
3. [第二步：同步到安全路径](#3-第二步同步到安全路径)
4. [第四步：编译](#4-第三步编译)
5. [第五步：检查错误](#5-第四步检查错误)
6. [第六步：烧录到设备](#6-第五步烧录到设备)
7. [关键约束（必读）](#7-关键约束必读)
8. [本次踩坑备忘](#8-本次踩坑备忘)
9. [快速参考：一键脚本](#9-快速参考一键脚本)

---

## 1. 环境概览

| 项目 | 路径 / 值 |
|------|-----------|
| Arduino CLI 可执行文件 | `C:\cardputer\tools\arduino-cli\arduino-cli.exe` |
| Arduino CLI 配置文件 | `C:\Users\87194\AppData\Local\Arduino15\arduino-cli.yaml` |
| esp32 核心版本 | **3.1.3-cn**（绝不能是 3.2.x / 3.3.x）|
| esp32 核心路径 | `C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.1.3` |
| FQBN | `esp32:esp32:m5stack_cardputer` |
| GitHub 源码路径 | `D:\github仓库同步\小机器\toolbox\toolbox.ino` |
| 编译用安全路径 | `C:\cardputer\sketches\toolbox\toolbox.ino` |
| 编译输出目录 | `C:\cardputer\build_fresh\out` |
| 编译日志（stdout） | `C:\cardputer\tools\tb.out.log` |
| 编译日志（stderr） | `C:\cardputer\tools\tb.err.log` |
| 设备串口 | `COM3`（M5Stack Cardputer-Adv）|

---

## 2. 第一步：编辑源码

直接编辑 GitHub 路径下的文件：

```
D:\github仓库同步\小机器\toolbox\toolbox.ino
```

**不要直接编辑** `C:\cardputer\sketches\toolbox\toolbox.ino`，那是编译副本，会被覆盖。

---

## 3. 第二步：同步到安全路径

Arduino CLI 在含中文字符的路径下有时会出错。编辑完成后，将源码同步到无中文的路径：

```powershell
# 确保目标目录存在
New-Item -ItemType Directory -Force "C:\cardputer\sketches\toolbox" | Out-Null

# 同步所有 .ino 和相关文件
Copy-Item -Force "D:\github仓库同步\小机器\toolbox\toolbox.ino" `
          "C:\cardputer\sketches\toolbox\toolbox.ino"
```

如果项目有多个文件（`.h`, `.cpp`），用：

```powershell
Copy-Item -Recurse -Force "D:\github仓库同步\小机器\toolbox\*" `
          "C:\cardputer\sketches\toolbox\"
```

---

## 4. 第三步：编译

**重要**：`arduino-cli.exe` 不在系统 PATH 里，必须用完整路径调用。

```powershell
$cli   = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"
$fqbn  = "esp32:esp32:m5stack_cardputer"
$sketch = "C:\cardputer\sketches\toolbox"
$build  = "C:\cardputer\build_fresh\out"
$outLog = "C:\cardputer\tools\tb.out.log"
$errLog = "C:\cardputer\tools\tb.err.log"

# 清空旧日志
"" | Out-File $outLog -Encoding utf8
"" | Out-File $errLog -Encoding utf8

# 编译（同步等待，输出写入日志）
$proc = Start-Process -FilePath $cli `
    -ArgumentList "compile --fqbn $fqbn --build-path $build $sketch" `
    -RedirectStandardOutput $outLog `
    -RedirectStandardError  $errLog `
    -NoNewWindow -PassThru -Wait

Write-Host "Exit code: $($proc.ExitCode)"
```

编译时间约 **60–120 秒**（首次更长，后续因缓存会快一些，但 `--build-path` 指定固定目录，每次都会重用已有缓存）。

---

## 5. 第四步：检查错误

```powershell
# 查看退出码（0 = 成功，非 0 = 失败）
Write-Host "Exit: $($proc.ExitCode)"

# 查看 stderr（编译错误在这里）
Get-Content "C:\cardputer\tools\tb.err.log"

# 查看 stdout（库版本、Used library 列表）
Get-Content "C:\cardputer\tools\tb.out.log"
```

### 常见错误及处理

| 错误信息 | 原因 | 解决方法 |
|----------|------|----------|
| `Platform 'esp32:esp32' not found` | 3.1.3 核心目录缺失 | 见下方「恢复核心」 |
| `esp8266-compat.h: No such file` | 误装了 3.3.x 核心 | 删除 3.3.x，只保留 3.1.3 |
| `esp_arduino_version.h: No such file` | 同上，3.3.x 混入 | 同上 |
| `Arduino.h: No such file` | 构建缓存污染 | 删除 `C:\cardputer\build_fresh\out` 再重编 |

#### 恢复 esp32 3.1.3-cn 核心（核心丢失时执行）

2026-05-23 实测：如果 `packages\esp32\tools` 还在，但
`packages\esp32\hardware\esp32\3.1.3` 不存在，说明只是 platform/hardware 目录缺失或损坏。
不需要先清理 tools，直接重装 `esp32:esp32@3.1.3` 即可。

```powershell
$cli = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"

# 确认配置中有这个可用镜像。不要使用 /arduino/arduino-esp32/...package_esp32_cn_index.json，
# 该 URL 于 2026-05-23 实测返回 401 Unauthorized。
& $cli config dump
& $cli config add board_manager.additional_urls `
    "https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json"

# 更新索引
& $cli core update-index

# 安装 3.1.3
& $cli core install esp32:esp32@3.1.3

Write-Host "完成。核心路径应为："
Write-Host "C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.1.3"
```

安装可能超过 5 分钟，尤其是首次恢复后要下载/解压数百 MB 工具包。即使命令超时，也先检查：

```powershell
Get-Process | Where-Object { $_.ProcessName -like "*arduino*" -or $_.ProcessName -like "*xtensa*" }
Test-Path "C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.1.3"
& $cli core list
```

#### 如果 3.3.x 被意外安装，必须手动删除

```powershell
# 检查是否存在 3.3.x
Test-Path "C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.8"

# 如果存在，删除它（arduino-cli uninstall 不支持版本参数，需手动删）
Remove-Item -Recurse -Force `
    "C:\Users\87194\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.8"

# 同时清理被污染的构建缓存
Remove-Item -Recurse -Force "C:\cardputer\build_fresh\out" -ErrorAction SilentlyContinue
```

---

## 6. 第五步：烧录到设备

编译成功后（exit code = 0），将 `.bin` 文件烧录到设备。

**烧录前：唤醒设备**  
如果设备处于深度睡眠状态，按一下键盘上的任意键将其唤醒，否则串口无法连接。

```powershell
$cli    = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"
$fqbn   = "esp32:esp32:m5stack_cardputer"
$build  = "C:\cardputer\build_fresh\out"
$port   = "COM3"
$outLog = "C:\cardputer\tools\tb.out.log"
$errLog = "C:\cardputer\tools\tb.err.log"

"" | Out-File $outLog -Encoding utf8
"" | Out-File $errLog -Encoding utf8

$proc = Start-Process -FilePath $cli `
    -ArgumentList "upload --fqbn $fqbn --port $port --input-dir $build" `
    -RedirectStandardOutput $outLog `
    -RedirectStandardError  $errLog `
    -NoNewWindow -PassThru -Wait

Write-Host "Upload exit code: $($proc.ExitCode)"
Get-Content $errLog
```

烧录约 **10–30 秒**。成功后设备自动重启。

---

## 7. 关键约束（必读）

### ⛔ 绝对不能升级 esp32 核心到 3.2.x / 3.3.x
M5Cardputer-Adv 的麦克风驱动（`Mic.begin()`）在 3.2.x+ 下会编译失败或运行时崩溃。  
只能使用 **3.1.3-cn**。如果 arduino-cli 提示更新核心，**拒绝**。

### ⛔ 不要把 arduino-cli 加入 PATH 后用 `arduino-cli compile`
用完整路径 `C:\cardputer\tools\arduino-cli\arduino-cli.exe` 调用，避免与系统其他版本混淆。

### ⛔ 不要直接在 D:\github仓库同步 路径下编译
中文路径在 arduino-cli 的某些子进程中会报错。始终先同步到 `C:\cardputer\sketches\toolbox`。

### ✅ 编译时始终用 --build-path
指定 `--build-path C:\cardputer\build_fresh\out` 可以：
- 避免使用被旧核心（3.3.x）污染的系统构建缓存
- 产生一个固定输出目录，方便 `upload --input-dir` 指向

### ✅ 先编译再烧录
不要直接用 `compile --upload`（一步完成），分两步更容易排查错误。

---

## 8. 本次踩坑备忘

| 坑 | 现象 | 处理 |
|----|------|------|
| esp32 platform 缺失 | `Platform 'esp32:esp32' not found`，`core list` 显示 `No platforms installed` | 直接 `core install esp32:esp32@3.1.3`，不用清理残留 tools |
| 错误 jihulab URL | `package_esp32_cn_index.json` 下载返回 `401 Unauthorized` | 使用 `https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json` |
| 沙箱/权限限制 | 访问 `Arduino15`、配置文件、串口时出现 `Access is denied` | 用外部权限执行 arduino-cli；不要误判为代码错误 |
| 一键脚本编码损坏 | `build_and_flash.ps1` 报字符串缺少结束符，输出中文乱码 | 暂用手动三步；必要时用本文第 9 节的 UTF-8 脚本重写 |
| 首次恢复后编译很慢 | `compile` 超时但后台仍有 `arduino-cli` / `xtensa` 进程 | 不要立刻重复启动多次，先等进程结束；第二次有缓存后会快很多 |
| 只编译不等于写进机器 | 编译成功但设备没有变化 | 还要执行 `upload --port COM3 --input-dir C:\cardputer\build_fresh\out` |

2026-05-23 烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Wrote 1143648 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 清理启动页冗余后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1142496 bytes (87%)
Global variables use 73636 bytes (22%)
Wrote 1142880 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 播放页 Del 二次确认删除后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1142736 bytes (87%)
Global variables use 73636 bytes (22%)
Wrote 1143120 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 统一长按 Del 3 秒删除、播放完自动回列表后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1143084 bytes (87%)
Global variables use 73636 bytes (22%)
Wrote 1143472 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 录音 Space 暂停/继续、Esc 息屏后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1143684 bytes (87%)
Global variables use 73636 bytes (22%)
Wrote 1144064 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 播放页按键统一、长按 Del 2 秒删除后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1143780 bytes (87%)
Global variables use 73636 bytes (22%)
Wrote 1144160 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 优化播放入口延迟后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1143712 bytes (87%)
Global variables use 73700 bytes (22%)
Wrote 1144096 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 恢复播放 A线信息、长按 Del 1.2 秒后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1144304 bytes (87%)
Global variables use 73700 bytes (22%)
Wrote 1144688 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 增加按键即时反馈后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1145088 bytes (87%)
Global variables use 73700 bytes (22%)
Wrote 1145472 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 息屏回车进入列表后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1145148 bytes (87%)
Global variables use 73700 bytes (22%)
Wrote 1145536 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 优化 A线样式、录音回列表、列表删除等待后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1145580 bytes (87%)
Global variables use 73700 bytes (22%)
Wrote 1145968 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 播放 A线单一数据源后烧录成功记录：

```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1144972 bytes (87%)
Global variables use 74724 bytes (22%)
Wrote 1145360 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

2026-05-23 录制/播放 A线统一小柱状倍率后烧录成功记录：
```text
Serial port COM3
Chip is ESP32-S3
Sketch uses 1144976 bytes (87%)
Global variables use 74724 bytes (22%)
Wrote 1145360 bytes
Hash of data verified.
Hard resetting with RTC WDT...
```

## 9. 快速参考：一键脚本

将以下内容保存为 `C:\cardputer\tools\build_and_flash.ps1`，需要时直接运行：

```powershell
# build_and_flash.ps1
# 用法：powershell -File C:\cardputer\tools\build_and_flash.ps1

$cli    = "C:\cardputer\tools\arduino-cli\arduino-cli.exe"
$fqbn   = "esp32:esp32:m5stack_cardputer"
$sketch = "C:\cardputer\sketches\toolbox"
$build  = "C:\cardputer\build_fresh\out"
$port   = "COM3"
$outLog = "C:\cardputer\tools\tb.out.log"
$errLog = "C:\cardputer\tools\tb.err.log"

# ── 1. 同步源码 ──────────────────────────────────────────
Write-Host "[1/3] 同步源码..."
New-Item -ItemType Directory -Force $sketch | Out-Null
Copy-Item -Force "D:\github仓库同步\小机器\toolbox\toolbox.ino" "$sketch\toolbox.ino"

# ── 2. 编译 ──────────────────────────────────────────────
Write-Host "[2/3] 编译中（需要 60-120 秒）..."
"" | Out-File $outLog -Encoding utf8
"" | Out-File $errLog -Encoding utf8

$proc = Start-Process -FilePath $cli `
    -ArgumentList "compile --fqbn $fqbn --build-path $build $sketch" `
    -RedirectStandardOutput $outLog `
    -RedirectStandardError  $errLog `
    -NoNewWindow -PassThru -Wait

if ($proc.ExitCode -ne 0) {
    Write-Host "[错误] 编译失败，退出码 $($proc.ExitCode)"
    Write-Host "--- stderr ---"
    Get-Content $errLog
    exit 1
}
Write-Host "[2/3] 编译成功 ✓"

# ── 3. 烧录 ──────────────────────────────────────────────
Write-Host "[3/3] 烧录到 $port（请先确认设备已唤醒）..."
"" | Out-File $outLog -Encoding utf8
"" | Out-File $errLog -Encoding utf8

$proc = Start-Process -FilePath $cli `
    -ArgumentList "upload --fqbn $fqbn --port $port --input-dir $build" `
    -RedirectStandardOutput $outLog `
    -RedirectStandardError  $errLog `
    -NoNewWindow -PassThru -Wait

if ($proc.ExitCode -ne 0) {
    Write-Host "[错误] 烧录失败，退出码 $($proc.ExitCode)"
    Get-Content $errLog
    exit 1
}
Write-Host "[3/3] 烧录成功 ✓  设备正在重启..."
```

---

## 附录：库版本（已验证可用）

| 库 | 版本 |
|----|------|
| M5Cardputer | 1.1.1 |
| M5Unified | 0.2.15 |
| M5GFX | 0.2.21 |
| esp32:esp32 core | **3.1.3** |

这些版本组合已通过完整编译验证。不要随意升级，尤其是 esp32 core。
