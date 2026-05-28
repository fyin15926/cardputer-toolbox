# 当前状态备忘

最后整理：2026-05-26

这个文件是新窗口入口。详细历史仍保留在根目录 `项目备忘.md`，但日常接手先看这里。

## 项目是什么

这是 M5Stack Cardputer-Adv 的录音工具箱项目，包含三块：

- `toolbox/toolbox.ino`：Cardputer 固件，负责录音、播放、列表、Wi-Fi 配置、上传、ADPCM 长录音压缩。
- `cloud-voice-server/server.js`：云端接收 WAV/ADPCM、转写、DeepSeek 整理、flomo 推送、后台试听和调参。
- `docs/`：交接、部署、踩坑和历史方案。

## 当前主线

- 固件端：当前重点是源头减少机器底噪。`recGain` 默认已降到 `36`，`processMicBuffer()` 增加了轻量低频底座跟踪/扣除。已编译通过，但这条固件改动仍需真机烧录后验证。
- 服务端：当前播放试听算法是 `smooth-play-preview-four-button-v16-spectral-profile`，使用 `REC_0045` 作为机器底噪参考，做频谱机器噪声指纹扣减。
- 转写整理：新录音默认 `ASR_SPEAKER_COUNT=2`，DeepSeek 默认模型为 `deepseek-v4-pro`，thinking 默认开启；多人对话、外语翻译、待办提取都有硬规则兜底。
- 原始 WAV 必须永久保留。服务器试听版、ASR-clean、DeepSeek 整理都只能作为派生结果，不要覆盖 raw。

## 需要验证

- 烧录 `toolbox/toolbox.ino` 后，录一条“蒙在被子里/纯机器底噪”样本，对比原始 WAV 底噪是否比之前低。
- 再录一条正常说话样本，确认 `recGain=36` 是否导致人声偏小；如果偏小，先用录音时 `W` 调高，不急着改服务器算法。
- 在后台用同一条录音比较原档、一档、二档、三档，判断 v16 频谱降噪是否继续作为网页试听基线。
- 单人备忘是否会被默认 2 人分离误拆。如果误拆明显，考虑把默认改回自动/不分角色，只用口令触发多人。

## 常用命令

固件固定入口：

```powershell
powershell -ExecutionPolicy Bypass -File "D:\github仓库同步\小机器\tools\cardputer_build.ps1"
powershell -ExecutionPolicy Bypass -File "D:\github仓库同步\小机器\tools\cardputer_build.ps1" -Flash -Port COM3
```

服务端本地语法检查：

```powershell
node --check cloud-voice-server\server.js
```

服务端本地启动：

```powershell
cd D:\github仓库同步\小机器\cloud-voice-server
npm start
```

## 不要重复踩的坑

- 固件必须用 `esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB`，默认分区会超限。
- 不要在中文路径里直接做 Arduino build 输出；脚本会同步到 `C:\cardputer\sketches\toolbox` 并输出到 `C:\cardputer\build_fresh\out`。
- 录音/播放实时内圈不能塞 Wi-Fi、NTP、HTTP、大 SD 读写或整屏刷新。
- 服务端默认 raw 优先。ASR-clean 和播放 preview 是实验/试听文件，不要自动替代最终转写，除非多样本证明更准。
- 密钥只放服务器环境变量或本机环境变量，不能写进 Git：`UPLOAD_TOKEN`、`ASR_FILE_TOKEN`、`DASHSCOPE_API_KEY`、`DEEPSEEK_API_KEY`、`FLOMO_WEBHOOK_URL`。

## 文件地图

- 总备忘：`项目备忘.md`
- 文档地图：`docs/README.md`
- 固件交接：`docs/firmware/HANDOVER.md`
- 编译烧录：`docs/firmware/COMPILE_FLASH.md`
- 服务器备忘：`docs/server/SERVER_MEMO.md`
- 部署说明：`docs/server/DEPLOYMENT.md`
- 踩坑日志：`docs/TROUBLESHOOTING_LOG.md`

## 建议整理项

- `项目备忘.md` 已经很长，后续只追加关键决策；日常状态同步优先更新本文件。
- `build_check/` 和 `build_backups/` 是本地构建/备份产物，已加入 `.gitignore`。
- `toolbox/backups/` 里有多份稳定固件快照，建议只保留重要里程碑，其余迁到外部归档或压缩包。
- `cloud-voice-server/server.20260525_stable_before_audio_modes.js` 是未跟踪备份文件，若确实有长期价值，移动到 `docs/archive/` 或 `cloud-voice-server/backups/` 并命名说明；否则不要提交。
