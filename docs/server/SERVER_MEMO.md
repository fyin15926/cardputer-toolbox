# Cardputer Voice Server 服务器备忘

最后更新：2026-05-25

## 服务器位置

- ECS 公网 IP：`47.110.91.244`
- 公网域名：`cardputer.flye.cc`
- 健康检查：`http://cardputer.flye.cc/health`
- 上传入口：`http://cardputer.flye.cc/upload`
- Job 查询：`http://cardputer.flye.cc/jobs/REC_XXXX`
- 旧任务重发到 flomo：`POST http://cardputer.flye.cc/jobs/REC_XXXX/resend`

## 运行目录

服务器主目录：

```text
/opt/cardputer-voice/
  server.js                         # 当前线上 Node 服务
  package.json
  README.md
  .env.example
  terms.json                        # 服务器端转写词典/错词修正
  uploads/                          # Cardputer 上传的 WAV
  jobs/                             # 每条录音的处理状态 JSON
  transcripts/                      # 原始转写、ASR JSON、flomo memo 版
  logs/
  server.js.bak-codex-format        # Codex 部署备份
  server.js.bak-codex-summary       # Codex 部署备份
  server.js.bak-codex-resend        # Codex 部署备份
```

当前线上已经出现 `REC_0041.memo.txt`、`REC_0042.memo.txt`、`REC_0043.memo.txt`，说明新格式 memo 落盘已经生效。

## systemd 服务

服务名：

```bash
cardputer-voice.service
```

关键配置：

```text
WorkingDirectory=/opt/cardputer-voice
EnvironmentFile=/etc/cardputer-voice.env
ExecStart=/usr/bin/node /opt/cardputer-voice/server.js
Restart=always
User=root
```

常用命令：

```bash
systemctl status cardputer-voice.service
systemctl restart cardputer-voice.service
journalctl -u cardputer-voice.service -f
node --check /opt/cardputer-voice/server.js
```

## 环境变量

真实值只在服务器 `/etc/cardputer-voice.env`，不要提交到 Git。

当前配置项名字：

```text
PORT
UPLOAD_TOKEN
MAX_UPLOAD_BYTES
DATA_ROOT
PUBLIC_BASE_URL
ASR_FILE_TOKEN
DASHSCOPE_API_KEY
DEEPSEEK_API_KEY
DEEPSEEK_BASE_URL
DEEPSEEK_MODEL
DEEPSEEK_TIMEOUT_MS
DEEPSEEK_MAX_TRANSCRIPT_CHARS
DEEPSEEK_SETTINGS_PATH
FLOMO_WEBHOOK_URL
```

## 数据文件含义

```text
uploads/REC_XXXX.wav
```

设备上传的原始 WAV。

```text
jobs/REC_XXXX.json
```

处理状态、录音名、设备名、上传时间、转写状态、flomo 发送状态等。

```text
transcripts/REC_XXXX.txt
```

DashScope 返回后提取出来的原始转写文本。

```text
transcripts/REC_XXXX.json
```

DashScope 原始 JSON 结果，用于排查 ASR 问题。

```text
transcripts/REC_XXXX.memo.txt
```

发给 flomo 的最终整理版，包含标题、摘要、待办/想法、分段原文和元信息。

## 录音处理原则：两套算法

后续音频优化要分清两个目标：

- 小机器端是“给人耳听”的处理：播放舒服、少刺耳、少刮擦、不要爆音，算法要轻，不能占太多设备资源。
- 服务器端是“给机器识别”的处理：目标是让 DashScope/ASR 更容易听清楚，识别更准，不要求处理后的声音最好听。

推荐保留三条线：

```text
raw.wav              # 小机器上传的原始录音，永远保留
play_preview.wav     # 人耳播放版，可由网页或小机器参数预览生成
clean_for_asr.wav    # 服务器识别版，送 DashScope 转文字
```

不要只保存处理后的文件。原始 WAV 是回退、对比和重新调参的基础。

## 网页后台不能做黑箱

后台页面除了显示 job 状态，还应该展示服务器处理过程，让用户知道每一步发生了什么。

2026-05-25 进展：任务详情页已经能显示上传格式、压缩倍率、WAV 采样信息、转写/memo 文件状态、原始音频播放、重跑转写和重发 flomo。音频实验区第一版已上线，可生成不覆盖原始 WAV 的 `previews/REC_XXXX.play-preview.wav`，并保存对应参数/统计到 `previews/REC_XXXX.play-preview.json`，用于和原始录音 A/B 试听。

建议每条录音显示：

- 原始录音：文件名、时长、大小、录音时间、播放按钮、波形。
- 小机器播放版：当前参数、处理后试听、刮擦声命中区域。
- 服务器识别版：使用的处理参数版本、处理日志、处理后的音频文件、处理前后音量变化、是否发现爆音或过静音。
- 语音转文字：原始录音直接识别结果、服务器处理后识别结果、两者差异、最终发到 flomo 的 memo。

这样网页可以作为“调音台 + 检查台”：既能调小机器播放效果，也能看服务器识别效果，避免后台只返回一个成功/失败状态。

## 后续调参工作流

第一阶段先做人工网页调参：

```text
小机器录音
-> 上传 raw.wav
-> 网页 A/B 试听原始版、人耳播放版、服务器识别版
-> 调整参数
-> Wi-Fi 推参数到小机器
-> 再录音验证
```

稳定后再做 AI 自动循环：

```text
录音 -> 上传 -> 分析录音和识别结果 -> 调参数 -> 推到小机器 -> 再录音
```

AI 自动调参也要以可见记录为准：每次改了哪些参数、为什么改、识别结果有没有变好，都要保存到 job 记录或调参日志里。

## flomo memo 整理逻辑

当前服务器端会在发 flomo 前做这些处理：

- DashScope 默认使用原始 WAV，`disfluency_removal_enabled` 默认为 `false`
- 读取 `/opt/cardputer-voice/terms.json` 做错词修正
- 如果配置了 `DEEPSEEK_API_KEY`，调用 DeepSeek 对 DashScope 原始文本做二次校正、断句、标题、摘要、待办和想法提取
- 后台页面的 `DeepSeek 设置` 模块可以直接改系统提示词、用户提示词、固定词、模型、温度、输出 token、超时和送入字符上限
- 设置保存到 `DATA_ROOT/deepseek-settings.json`（或 `DEEPSEEK_SETTINGS_PATH` 指定位置），下一次重跑/重发立刻生效，不用重启
- `保存并仅重跑 DeepSeek` / `POST /jobs/:id/polish` 只复用已有 `transcripts/REC_XXXX.txt` 重建 `polished.txt`、`deepseek.json` 和 `memo.txt`，不重新跑 DashScope，也不发 flomo
- `保存并重跑完整转写` / `POST /jobs/:id/process` 会重新生成 ASR-clean、重新提交 DashScope，然后再跑 DeepSeek
- DeepSeek 结果会保存到 `transcripts/REC_XXXX.deepseek.json`，校正文保存到 `transcripts/REC_XXXX.polished.txt`
- 如果 DeepSeek 超时或返回异常，自动回退到原有规则整理，避免阻断转写和 flomo
- 自动生成短标题：`#Cardputer语音 / 标题`
- 自动生成 `摘要`
- 如果命中关键词，自动生成 `待办` 和 `想法`
- 保留完整分段 `原文`
- 保存最终 memo 到 `transcripts/REC_XXXX.memo.txt`

## 词典文件

路径：

```text
/opt/cardputer-voice/terms.json
```

格式可以是数组：

```json
[
  ["浮墨", "flomo"],
  ["card puter", "Cardputer"]
]
```

改完词典后重启服务：

```bash
systemctl restart cardputer-voice.service
```

需要把旧任务按新词典重新发到 flomo：

```bash
curl -X POST http://cardputer.flye.cc/jobs/REC_0031/resend
```

注意：`/resend` 会真的向 flomo 再发一条新 memo，使用前确认不要重复刷屏。

## 本地部署命令参考

从本机部署当前代码：

```powershell
scp -i C:\tmp\cardputer_cloud_key cloud-voice-server\server.js root@47.110.91.244:/opt/cardputer-voice/server.js
scp -i C:\tmp\cardputer_cloud_key cloud-voice-server\terms.example.json root@47.110.91.244:/opt/cardputer-voice/terms.json
ssh -i C:\tmp\cardputer_cloud_key root@47.110.91.244 "node --check /opt/cardputer-voice/server.js && systemctl restart cardputer-voice.service && systemctl is-active cardputer-voice.service"
curl.exe -sS http://cardputer.flye.cc/health
```
