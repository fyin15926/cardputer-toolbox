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

## flomo memo 整理逻辑

当前服务器端会在发 flomo 前做这些处理：

- DashScope 开启 `disfluency_removal_enabled: true`
- 读取 `/opt/cardputer-voice/terms.json` 做错词修正
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
