# 踩坑与处理日志

最后更新：2026-05-25

这个文件专门记录已经踩过的坑、当时现象、真正原因和下次避免方式。开新窗口或换人接手时，先读 `项目备忘.md`，再读本文件。

## 总原则

- 先看现象属于哪一层：小机器固件、USB/串口、SD 配置、云服务器、浏览器后台、第三方转写/flomo。
- 不要把“编译成功”当成“已经写进机器”。必须看到 upload 的 `Hash of data verified` 和 `Hard resetting with RTC WDT`。
- 不要把真实密钥写进 Git。`UPLOAD_TOKEN`、`DASHSCOPE_API_KEY`、`FLOMO_WEBHOOK_URL` 只在服务器环境变量或本机私密配置里。
- 服务器重启会清空内存里的设备最近状态和 active uploads，但不会丢 `jobs/`、`uploads/` 里的历史任务。

## 2026-05-25：长录音上传慢

| 项目 | 记录 |
|---|---|
| 现象 | 28.2 MB WAV 只能约 89 KB/s 上传，预计 4 分多钟；小机器显示 `UP` 很久。 |
| 判断 | 不是单纯服务器限速，主要受小机器 Wi-Fi、SD 读取、HTTP 发送和 WAV 体积影响。 |
| 解决 | 超过 3 MB 的录音在小机器端用 IMA ADPCM 临时压缩上传，服务器收到后还原成标准 WAV；短录音仍直接传 WAV。 |
| 结果 | ADPCM 大约 4 倍压缩，后台显示 `ADPCM`、上传大小、原始大小、压缩倍率。 |
| 避免重复踩坑 | 长文件慢先看后台“格式”和“上传大小”；如果仍显示 WAV，说明机器端还没写入新固件或文件低于 3 MB 阈值。 |

## 2026-05-25：后台没有看到小机器上传

| 项目 | 记录 |
|---|---|
| 现象 | 小机器显示 `UP`，后台 active uploads 没反应。 |
| 判断 | 可能是设备还没真正连上服务器，也可能是旧的半截上传文件留在服务器，导致新请求被当成异常/重复。 |
| 解决 | 服务器端加入 stale partial upload 处理：如果 `uploads/REC_xxxx.wav` 存在但对应 job JSON 不存在，删除旧半截文件并允许重试。 |
| 避免重复踩坑 | 看 `/api/dashboard` 的 `activeUploads`、`devices.lastStatus` 和 `jobs`；若设备端一直 `UP` 但服务端没有 active upload，优先查服务器 job/upload 文件是否不一致。 |

## 2026-05-25：写入后黑屏或像没写进去

| 项目 | 记录 |
|---|---|
| 现象 | 编译/上传后机器不亮屏，或感觉“说写进去了但没变化”。 |
| 真正原因 | 曾经使用过不完整或不一致的 FQBN/分区参数；Cardputer-Adv 必须按 8 MB Flash + 3 MB APP 分区编译和烧录。 |
| 固定做法 | 编译和烧录都使用 `esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB`。 |
| 验证标准 | upload 输出里看到 ESP32-S3、Embedded Flash 8MB、写入 app、`Hash of data verified`、`Hard resetting with RTC WDT`。 |
| 避免重复踩坑 | 不要只看 compile 成功；必须再执行 upload。不要用默认 `m5stack_cardputer` FQBN。 |

## 2026-05-25：编译卡住、超时和一堆工具链进程

| 项目 | 记录 |
|---|---|
| 现象 | 修改 `toolbox/toolbox.ino` 后，第一次编译报 `Access is denied`、`Platform 'esp32:esp32' not found`，随后用授权方式重跑又连续超时；任务管理里残留 `arduino-cli` 和大量 `xtensa-esp32s3-elf-g++` / `xtensa-esp-elf-g++` 进程。 |
| 真正原因 | 第一次是沙箱/权限问题：`arduino-cli` 需要访问并写入 `C:\Users\87194\AppData\Local\Arduino15\packages/tmp`，普通沙箱命令无法跟随该目录和创建临时解压目录，所以误报平台不存在。后两次不是源码错误，而是并行编译在 Windows 上重编 M5GFX/M5Unified 等大量库时卡住/推进极慢，命令超时后子进程还残留。 |
| 解决 | 先停止残留的 `arduino-cli` 和 `xtensa-*g++` 进程，避免多轮编译叠在一起；把仓库里的 `toolbox.ino` 同步到 `C:\cardputer\sketches\toolbox\toolbox.ino`；然后使用固定 build path 和单任务编译：`arduino-cli compile --jobs 1 --fqbn esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB --build-path C:\cardputer\build_fresh\out C:\cardputer\sketches\toolbox`。 |
| 结果 | 单任务编译约 29 秒通过：程序约 1,746,088 bytes，占 52%；全局变量约 137,456 bytes，占 41%。随后用同一个 FQBN 和 `--input-dir C:\cardputer\build_fresh\out` 烧录 COM3 成功，日志包含 `Embedded Flash 8MB`、多段 `Hash of data verified`、`Hard resetting with RTC WDT`。 |
| 避免重复踩坑 | 遇到 `Access is denied` 或 `Platform 'esp32:esp32' not found` 先判断是不是权限/缓存目录问题，不要立刻改代码或重装 core；如果编译超时后仍有 `arduino-cli` / `xtensa-*g++`，先停掉残留进程再重跑；在这台 Windows 环境优先用 `--jobs 1`，稳定性比并行编译重要。 |

## 2026-05-25：COM3 打不开

| 项目 | 记录 |
|---|---|
| 现象 | `Could not open COM3`，`PermissionError(13, '连到系统上的设备没有发挥作用。')`。 |
| 判断 | 通常不是源码问题，也不是 flash 写坏；是 Windows 串口状态、USB 线/口、设备刚枚举不稳或被程序占用。 |
| 处理 | 先查 `arduino-cli board list`，再查是否有 `arduino/esptool/serial` 进程占用；没有占用时拔插 USB 后重试。 |
| 成功标准 | esptool 能进入 `Connecting...`、识别芯片、开始擦写。 |
| 避免重复踩坑 | 第一次失败不要立刻改代码；先处理串口和连接状态。 |

## 2026-05-25：云端后台 `/dashboard` 打不开或空白

| 项目 | 记录 |
|---|---|
| 现象 | 访问 `https://cardputer.flye.cc/dashboard` 显示 route 不存在，或后台页面只有空白/读取失败。 |
| 原因 | 早期服务器没有 dashboard route；后来 token 不正确时 API 返回 401，页面提示读取失败。 |
| 解决 | 增加 `/dashboard`、`/api/dashboard`、job 详情页；前端保存 token 到浏览器 localStorage，并把 `UPLOAD_TOKEN=...` 这种粘贴格式自动清理成纯 token。 |
| 避免重复踩坑 | 后台 token 只粘贴等号后面的值；如果服务器刚重启，设备状态可能暂时为空，等下一次上传会恢复。 |

## 2026-05-24：编译环境与核心版本

| 项目 | 记录 |
|---|---|
| 现象 | `Platform 'esp32:esp32' not found`，或新核心下麦克风录音静音/平线。 |
| 原因 | esp32 core 缺失或版本不合适；3.3.x 上有当前项目不能接受的 I2S/录音问题。 |
| 固定做法 | 使用 esp32 core 3.1.3；国内下载优先走 Jihulab 镜像和预取缓存。 |
| 避免重复踩坑 | 不要为了“最新版”升级到 3.3.x；升级前必须真机验证录音、播放、键盘、Wi-Fi 上传。 |

## 2026-05-24：M5Cardputer 键盘库中断问题

| 项目 | 记录 |
|---|---|
| 现象 | 开机重启/闪屏，系统 ipc1 任务栈异常。 |
| 原因 | 原版键盘库使用 GPIO 中断，在当前核心和项目组合下不稳定。 |
| 解决 | 把键盘读取改成轮询。 |
| 避免重复踩坑 | 如果重装 M5Cardputer 库，这个本地改动可能丢失，需要重新检查 `KeyboardReader/TCA8418.cpp`。 |

## 2026-05-24：音频处理方向踩坑

| 项目 | 记录 |
|---|---|
| 现象 | 强 AGC、强语音门、整块衰减会让人声变糊、断续，且不一定能抓到按键摩擦。 |
| 结论 | 小机器端做轻量、人耳舒服的处理；服务器端另做 ASR 识别优化。两套目标不能混在一个算法里。 |
| 当前做法 | 小机器保存后轻量削高频摩擦；原始 WAV 永远保留；后续网页做 A/B 对比和参数记录。 |
| 避免重复踩坑 | 不要为了“降噪”直接上重处理；每次调参都保留原始音频、参数版本和识别结果。 |

## 2026-05-25：录音/播放实时链路卡顿和屏幕闪烁

| 项目 | 记录 |
|---|---|
| 现象 | 录音时屏幕频闪严重，录出来的音频一卡一卡；播放页有时卡、有时正常；后来又出现底部时间数字闪、开始录音/暂停继续/长按删除时整页闪。 |
| 真正原因 | 录音实时内圈里同时做了麦克风采样、SD 写入、显示刷新和部分上传/联网等待，资源互相抢；显示侧还存在全屏 `fillScreen` / 全屏 `pushSprite`，以及底部数字直接在屏幕上先清黑再逐段画的问题。 |
| 解决 | 录音先处理当前 mic buffer，立刻重新 `Mic.record()` 续采样，再批量写 SD；波形区拆成 `WAVE_TOP..WAVE_BOT` 局部 canvas，录音/播放 A/B 线约 60fps 局部刷新；底部时间栏单独用小 canvas 离屏画好再推送；开始录音、暂停/继续、删除进度也只刷顶栏/底栏局部区域。 |
| 上传策略 | 不做真正并行的“录音时 Wi-Fi 上传”，因为会重新抢 SD/Wi-Fi/CPU 并引发卡音。当前采用协作式后台队列：列表页按上传只入队；空闲时慢慢传；有按键就暂停当前尝试但保留 `UP`；录音/播放期间不跑上传。 |
| 结果 | 录音启动无明显延迟；录音和播放 B 线保持流畅；底部数字、开始录音、暂停/继续、长按删除都不再整页闪；用户确认体验明显改善。 |
| 避免重复踩坑 | 不要把 `M5Cardputer.update()`、NTP、Wi-Fi connect、HTTP upload、SD 大块读写、整屏 `pushSprite(0,0)` 放进录音/播放实时路径。若要提升显示体验，优先做局部 sprite 和状态区分层，而不是提高整屏刷新率。 |

## 运行日志位置

- Git 历史：`git log --oneline`
- 固件编译/烧录说明：`docs/firmware/COMPILE_FLASH.md`
- 固件当前状态：`docs/firmware/HANDOVER.md`
- 服务器运行备忘：`docs/server/SERVER_MEMO.md`
- 服务器线上日志：`journalctl -u cardputer-voice.service -f`
- 服务器任务数据：`/opt/cardputer-voice/jobs/`
- 服务器上传音频：`/opt/cardputer-voice/uploads/`
