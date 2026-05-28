# WebSocket 实时语音重写交接

## 2026-05-27 本轮收尾交接：CHAT 体验与米家/Home Assistant 结论

- 本轮 CHAT 体验已经围绕“像正常助手”继续收敛：小猪字符画换行和停留已修；输入 `YOU` 可见；回复 `REPLY` 播放完不再被 `READY` 覆盖；`READY` 只在右下角状态位显示。
- 服务端已改为流式 TTS：`reply_text` 发出后立即进入 TTS 推流，避免等完整 WAV 合成后才开始说话。若后续仍觉得语音慢，优先看 DashScope TTS 首包耗时和服务器 `tts_start/audio_start` 日志。
- DeepSeek 链路按官方文档校正：线上使用 `DEEPSEEK_MODEL=deepseek-v4-pro`；CHAT 普通对话显式 `thinking: { type: 'disabled' }`；不再强制 `response_format: json_object`；创意请求高温，事实/普通请求中低温。
- 规则层已大幅削弱：主路径只保留小动物/ASCII 绘图、简单算术、日期星期、明确实时信息不可用提醒。普通聊天、生活问题、解释、笑话、追问尽量交给 DeepSeek，避免助手被规则绑死。
- 绘图规则已修正：`再来一个新的笑话`、`换一个更好笑的`、`再来一个问题` 不再被小猪上下文抢走；明确 `给我画小猪/换一个小猪` 才走 ASCII 绘图。
- ASR 已加收尾保护：实时 ASR 只有 `sentence_end` 且不像半句话时才直接采用，否则回退完整 WAV ASR，避免“给我画一只小猪”被截成“给我画一只小”后乱澄清。
- CHAT 右上角最终只显示电量，颜色跟随 CHAT 主题色；不再显示时钟。`Esc` 在 CHAT 内已改为清理资源后 `APP_SLEEP` 息屏。
- 米家/Home Assistant 结论：Cardputer 本体不能装 Home Assistant，它是 ESP32 小设备，适合当遥控器；Home Assistant 应装在家里常开的 Windows 电脑/NAS/树莓派/x86 小主机/虚拟机/Docker 上。未来控制米家建议走 `Cardputer -> 云服务器 -> 家里 Home Assistant -> 米家设备`，不建议直接逆向米家云账号。
- 新窗口建议先验真机：画小猪/换小猪、多次讲笑话“再来一个”、生活类问答、文字保留、语音开始速度、播放结束右下角 `READY`、`Esc` 息屏。

## 2026-05-27 CHAT Esc 息屏

- 用户反馈：按 `Esc` 为什么不是息屏，要求改好。
- 根因：CHAT 页 `keyEsc()` 分支清理连接后返回 `APP_LAUNCHER`，因此回工具箱而不是睡眠。
- 固件修正：`chatStreamScreen()` 中 `Esc` 分支保持原有清理流程（停止上行、停止播放任务、停喇叭、释放 PCM ring、断开 WebSocket、关闭 Wi-Fi、等待松键），最后改为 `return APP_SLEEP`。
- 验证：`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1763064 bytes`，全局变量 `145832 bytes`；已刷入 COM3，写入 hash 校验通过。无需改服务器。

## 2026-05-27 CHAT 电量主题色

- 用户反馈：CHAT 右上角电量仍是录音工具绿色，没有改成 CHAT 主题色。
- 固件修正：
  - 新增 `drawChatBattery()`，复用 `filteredBatteryLevel()` 和数码管字体。
  - CHAT 正常电量使用当前 `chatAccentColor()`，低电量 `<=20%` 保持红色警告。
  - `drawChatHeader()` 改为调用 `drawChatBattery()`，不再调用全局 `drawStatusBattery(d)`。
- 验证：`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1763060 bytes`，全局变量 `145832 bytes`；已刷入 COM3，写入 hash 校验通过。无需改服务器。

## 2026-05-27 CHAT 画图规则不再抢“再来/换一个”

- 用户质疑：`再来`、`换一个` 是日常词，不可能不说；如果这些词被画图规则抓走，就是限制助手自主能动性。判断正确。
- 说明：温度是采样随机性。高温更发散、更适合笑话/故事；低温更稳定、更适合事实/计算/安全提醒。当前策略保留“创意类高温、事实类中低温”，不全局高温，避免事实题飘。
- 服务端修正：
  - `asciiArtChatReply()` 新增门槛：只有当前文本明确包含画图意图、明确动物名，或是非常短的画图 follow-up，才可能进入画图规则。
  - 创意类请求直接排除画图规则。
  - 普通 `再来一个问题`、`再来一个新的笑话`、`换一个更好笑的` 不再被小猪上下文截走。
- 线上 probe：
  - `再来一个新的笑话` -> `provider: deepseek`
  - `换一个更好笑的` -> `provider: deepseek`
  - `再来一个问题` -> `provider: deepseek`
  - `给我画一只小猪` -> `provider: rules`
  - 小猪上下文后的短句 `换一个` -> `provider: rules`
  - `给我换一个小猪` -> `provider: rules`
- 部署：远端 `node --check` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 笑话重复修正

- 用户反馈：让小机子讲笑话时，来来回回都是一两个笑话。
- 复查发现两个原因：
  - CHAT 普通对话温度过低，之前为稳定性设为 `0.2`，创意任务容易重复安全老梗。
  - 小猪/ASCII 上下文规则会把 `再来一个新的笑话`、`换一个更好笑的` 里的 `再来/换一个` 误判为“换小猪”。
- 服务端修正：
  - 新增 `isCreativeChatRequest()`，识别笑话、段子、故事、幽默等创意请求。
  - 创意请求使用 `temperature: 0.85`、`top_p: 0.95`；普通请求使用 `temperature: 0.35`、`top_p: 0.8`。
  - prompt 明确要求：笑话/创意请求不能重复最近上下文里的笑话或包袱；不要默认讲程序员/数学冷笑话，除非用户明确要编程幽默；优先日常生活、食物、职场、学校或荒诞小场景。
  - `asciiArtChatReply()` 遇到创意请求直接返回 null，避免“再来/换一个”被画图上下文截走。
- 线上 probe：
  - `讲一个很好笑的笑话`
  - `再讲一个，不要重复`
  - `再来一个新的笑话`
  - `换一个更好笑的`
  四轮均为 `provider: deepseek`，内容不同，未再触发小猪规则。
- 部署：备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-creative-jokes`；远端 `node --check` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 右上角改为电量

- 用户询问时钟是否消耗资源，并表示如果多就去掉，右上角显示电量即可。
- 结论：时钟资源消耗很小，但会占空间且依赖时间同步；按用户偏好改为电量。
- 固件修正：
  - 移除 `drawChatClock()` 和 CHAT 主循环 30 秒时钟刷新。
  - `drawChatHeader()` 改为调用已有 `drawStatusBattery(d)`，右上角显示滤波后的电量百分比。
- 验证：`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762880 bytes`，全局变量 `145832 bytes`；已刷入 COM3，写入 hash 校验通过。无需改服务器。

## 2026-05-27 CHAT 顶栏时钟与 READY 角标

- 用户反馈：
  - CHAT 工具右上角没有时钟，而录音工具有。
  - 回复语音播完后右下角没有显示 `READY`。
- 固件修正：
  - 新增 `drawChatClock()`，在 CHAT 顶栏右上角显示 `HH:MM`；无时间时显示 `--:--`。
  - `drawChatHeader()` 调用 `drawChatClock()`；CHAT 主循环中每 30 秒刷新一次时钟。
  - `drawChatCornerStatus()` 从屏幕底边上移到 y=118，并使用当前 CHAT 亮色而不是暗色，避免右下角 `READY` 看不清或被裁掉。
- 验证：`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1763256 bytes`，全局变量 `145832 bytes`；已刷入 COM3，写入 hash 校验通过。无需改服务器。

## 2026-05-27 CHAT TTS 后保留回复正文

- 用户反馈：说话结束后屏幕仍会从回复文字变成整屏 `READY`；期望 `READY` 只在右下角显示，正文保留。
- 根因：服务端切到流式 TTS 后，固件收到 TTS `audio_start` 时调用 `chatPcmReset()`，该函数会把 `chatTextPageVisible=false`。播完后 `audio_end`/`chatReadyAfterPlayback` 看到正文状态为 false，于是整屏 `drawChatStreamHome("READY")` 覆盖了回复。
- 固件修正：
  - `chatPcmReset(bool keepTextPage = false)` 新增参数。
  - TTS `audio_start` 路径调用 `chatPcmReset(chatTextPageVisible && chatTtsPlaying)`，重置 PCM 缓冲但保留回复正文可见状态。
  - 新一轮录音、连接初始化等默认仍清正文。
- 验证：`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762872 bytes`，全局变量 `145832 bytes`；已刷入 COM3，写入 hash 校验通过。无需改服务器。

## 2026-05-27 CHAT 流式 TTS 下发

- 用户反馈：文字回复出现后，还要等很久才开始说话，希望文字出来后马上开始语音。
- 根因：旧链路 `reply_text -> synthesizeChatReplyWav() -> 收完整 DashScope PCM -> 写 WAV -> 读 WAV -> 重采样 -> sendChatTtsAudio()`，虽然 DashScope TTS WebSocket 本身会边生成边吐二进制音频，但服务端一直等 `task-finished` 后才开始给 Cardputer 发音频。
- 已新增 `streamChatTtsAudio(clientWs, text, id, shouldStop)`：
  - TTS 直接请求 `16kHz pcm`，避免先生成 24k WAV 再重采样。
  - 收到第一批二进制 PCM 后立刻发送 `audio_start` 和 WebSocket binary 音频帧。
  - 继续按 512 samples 分块下发；开头 preroll 仍快速填充，后续按 32ms pacing。
  - `replyToStreamCapture()` 现在在 `reply_text` 后直接 `tts_start` 并调用 `streamChatTtsAudio()`，不再等待完整 WAV。
- 旧 `synthesizeChatReplyWav()` / `sendChatTtsAudio()` 暂时保留，供 HTTP CHAT 或回退路径使用。
- 部署：备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-streaming-tts`；本地和远端 `node --check` 均通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。
- 待真机验证：问一句较长回复，观察日志中 `reply ...` 后应很快出现 `tts stream audio_start ...`，屏幕文字应先出现，随后很快开始语音，不再等整段 WAV 生成完成。

## 2026-05-27 CHAT 仅保留必要规则层

- 用户追问“到底加了多少规则”，并明确希望是智能助手，而不是被限定在规则里的助手。复查代码后确认此前至少有三层限制：ASCII 绘图规则、`tryFastChatReply()` 大快答规则、坏回复/复读兜底；其中大快答规则包含机票、黄金、距离、吃饭、日期、自我介绍、thinking、百年孤独等，确实过度截胡。
- 已改主路径：`replyToChatWithDeepSeek()` 不再调用 `tryFastChatReply()`，改为调用新的 `tryEssentialChatReply()`。
- 现在主路径规则只保留：
  - ASCII/小动物字符画，因为必须控制 `displayText/speakText`，防止 TTS 读字符画。
  - 简单算术。
  - 明确日期/星期/几号。
  - 明确实时价格不可查的机票/金价提示。
- 午饭、路线距离、百年孤独、为什么笨、普通知识/建议题现在都交给 DeepSeek 自由理解。
- Prompt 补充：用户批评“笨/答错”时，不要甩锅给用户或要求用户换问法；要承认失败、说明可能原因，并尽量回答当前意图。
- 线上 probe：
  - `今天中午吃啥？` -> `provider: deepseek`，给盖饭/面条/饺子等选择。
  - `帮我查一下广州到太原一共多少公里？` -> `provider: deepseek`，回答公路约1900公里、直线约1600公里。
  - `用100个字介绍百年孤独` -> `provider: deepseek`，不再走固定百年孤独规则。
  - `今天广州到太原的机票大概多少钱？` -> `provider: rules`，只因实时票价确实不可查。
- 部署：备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-essential-rules-only`；远端 `node --check` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 规则层收窄，让模型自由理解

- 用户判断：小机子的“智商没有在线”，怀疑我们给了太多限制，没有让大模型自由发挥。复查最新记录后确认这个判断基本正确：很多答非所问不是 DeepSeek 生成的，而是前置 `rules` 快答层截胡。
- 最新坏例子：
  - `今天广州到太原的机票大概多少钱？` / `我说的是帮我查一下广州到太原的机票多少钱？` 被距离规则抢答成“直线约1600公里”。
  - `今天中午吃啥？` 之前被规则固定回答；放给 DeepSeek 后又因提示词过度保守说“打开地图或外卖软件”。
- 已修正方向：规则层只保留确定性、低风险能力；开放问题尽量交给 DeepSeek。
  - 机票/航班/票价/多少钱这类实时价格请求优先明确说明“当前不能查实时票价，需要手机查航旅平台；我只能给路线和大概判断”，不再误触距离规则。
  - 午饭/日常建议不再由规则固定回答，交给 DeepSeek。
  - Prompt 新增：日常建议题不要因为缺少实时位置/app 数据就拒答，要给 2-3 个合理选择，再说明补充什么信息能更准。
- 线上 probe：
  - `今天中午吃啥？` -> DeepSeek 给通用建议：食堂、盖饭或面馆，并提示位置/预算能更准。
  - `我现在有点饿但不想太麻烦` -> DeepSeek 给速食、便利店、外卖等选择。
  - `今天广州到太原的机票大概多少钱？` -> 明确不能查实时票价，不再回答距离。
- 部署：备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-rule-minimal`；远端 `node --check` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 半截 ASR 不再直接回答

- 用户反馈：看对话记录后感觉“智商没有在线”。复查真实线上记录发现更深层根因：服务端过度相信 DashScope realtime ASR 的中间结果，很多句子是半截就被拿去回答：
  - `帮我查一下太原到广州一共有多` -> 旧回复只会说“语音断了”。
  - `帮我我查一下广州到太原一共多` -> 旧回复只会澄清距离/航班。
  - `给我介绍一`、`画一只小`、`只小` 等明显断尾也会直接进入回复链路。
- 已修正 realtime ASR 采用策略：
  - `createRealtimeAsrSession()` 记录 `capture.realtimeFinal = sentence.sentence_end`。
  - `finalizeStreamAsr()` 只有在 `realtimeFinal=true` 且 `looksIncompleteChatTranscript()` 判定不是断尾时，才直接用 realtime 文本。
  - 否则回退到完整 WAV 文件 ASR，再进入 `replyToStreamCapture()`，牺牲一点速度，优先保证不要拿半句话答非所问。
- 新增 `looksIncompleteChatTranscript()`，识别过短文本、纯数字、以 `一共多/多少/多久/多远/吃什么/可以/给我介绍一/画一只小/换一只/就会/只小` 等断尾结尾的输入。
- Prompt 已调整：不再要求模型对所有不完整 ASR 只澄清；如果意图明显，应先给 best-effort 答案并带短 caveat。
- 快答补充：`太原到广州/广州到太原 ... 一共多/多少/距离/多久` 直接给大致距离，并说明高铁/航班需实时查询。
- 线上 probe 已验证：
  - `帮我查一下太原到广州一共有多` -> 给太原到广州大致距离，不再只说语音断了。
  - `帮我我查一下广州到太原一共多` -> 同上。
  - `今天中午吃啥？` -> 午饭建议。
  - `给我介绍一` -> 仍澄清“介绍什么”，因为缺少对象。
- 部署：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-asr-complete-before-reply`；`node --check` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 快答层误判修正

- 用户反馈：仍然“答非所问”。复查线上日志确认核心问题不是这轮 DeepSeek，而是服务端快答/绘图规则截胡：
  - `今天中午吃啥？`、`告诉我哦，我今天中午可以`、`我今天中午` 被旧日期快答误判为问日期，回复成 `今天是2026年5月27日，星期三。`
  - `我说的是帮我查一下今天的黄金` 也被旧日期快答截胡。
  - `给我看一下今天是什么日子，然后给我换一个` 在有小猪上下文时被绘图规则误判为“换小猪”。
- 已修正：
  - 黄金/金价请求优先回复“当前不能查实时金价，并询问国际金价、国内金价还是首饰回收价”。
  - 午饭/吃什么请求优先给吃饭建议。
  - 日期快答只处理明确的 `星期/几号/日期/什么日子` 意图，不再吃掉 `今天中午/今天黄金/今天可以`。
  - 绘图规则遇到明确日期问题且没有明确动物名时不再用最近小猪上下文抢答。
- 线上 probe 已验证：
  - `给我看一下今天是什么日子，然后给我换一个` -> `今天是2026年5月27日，星期三。`
  - `今天中午吃啥？` -> 给午饭建议。
  - `我说的是帮我查一下今天的黄金` -> 不再回日期，改问要哪类金价。
- 部署：远端 `node --check /opt/cardputer-voice/server.js` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。无需刷固件。

## 2026-05-27 CHAT 输入可见与回复保留

- 用户明确需求：必须能在屏幕上看见自己输入/语音识别出来的话；小机子回复完成后，回复文字不要消失；如果要显示 `READY`，只能显示在右下角，不能覆盖正文。
- 固件已改：收到 `asr_text` 时绘制 `YOU` 页面显示用户原话；收到 `reply_text` 时绘制 `REPLY` 页面显示小机子回复。
- 固件状态显示已改为角落状态：`reply_start`、`tts_start`、播放结束后的 `READY` 都优先调用右下角小状态，不再用整屏 `THINKING/SPEAKING/READY` 覆盖 `YOU` 或 `REPLY` 正文。
- 只有开始新一轮录音时才清掉旧正文并进入 `LISTEN`。回复播完后正文保留，右下角显示 `READY`。
- 服务端已补发最终用户输入：`replyToStreamCapture()` 在进入 LLM 前会发送一条 `asr_text final:true`，即使实时 ASR 中间结果没到，固件也能显示用户原话。
- 验证与部署：本地 `node --check cloud-voice-server/server.js` 通过；固件 `powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762856 bytes`，全局变量 `145832 bytes`；服务器备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-show-user-text`，上传后远端 `node --check` 通过，服务重启 `active`，`/health` 正常；固件已刷入 COM3，写入 hash 校验通过。

## 2026-05-27 CHAT DeepSeek 官方参数校正

- 用户反馈：CHAT 仍然“很傻、答非所问”，并要求核对 DeepSeek 官方文档。
- 已查官方文档并修正：`deepseek-v4-pro` 模型名是对的，但 DeepSeek V4 `thinking` 默认开启；thinking 模式下 `temperature/top_p/presence_penalty/frequency_penalty` 不生效，且会增加响应耗时。CHAT 普通对话现已显式传 `thinking: { type: 'disabled' }`。
- 已去掉 CHAT 普通对话的强制 `response_format: { type: 'json_object' }`。官方 JSON Output 文档说明该模式可能偶发空内容；当前只在 prompt 中要求 JSON，服务端仍能解析 JSON，若模型返回普通文本则兜底当作 `replyText`，避免空回复和卡顿。
- CHAT 普通对话参数改为 `temperature: 0.2`、`max_tokens: 380`、`stream: false`。短问答/绘图/算术/日期等仍优先走规则层，复杂开放问题才走 DeepSeek。
- 已修正绘图规则抢答：类似“我让你画小猪你为什么问小狗”这类疑问句不再被 ASCII 绘图规则直接截走，而是交给 DeepSeek 解释；“我说让你画小猪，不是小狗”仍会直接画小猪。
- 线上已部署并重启：备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-deepseek-thinking-disabled`；`node --check /opt/cardputer-voice/server.js` 通过；`cardputer-voice.service` 为 `active`；`/health` 正常。
- 线上 probe 验证：
  - `我让你画小猪你为什么问小狗` -> DeepSeek 回复解释“没有问小狗，可能是语音识别偏差”。
  - `你确定大模型设置好了吗？` -> DeepSeek 回复“大模型连接正常...”。
  - `我说让你画小猪，不是小狗` -> 规则层直接返回多行 ASCII 小猪，TTS 只说“画好了。”。

## 2026-05-27 TTS paced 服务端验证

- 已确认线上 `cardputer-voice.service` 为 `active`，`/opt/cardputer-voice/server.js` 包含 paced 下发逻辑：`prerollSamples=12288`，`pacedDelayMs=30`。
- 真机触发后，服务端日志显示中短回复闭环正常：`20260526T165646Z_cardputer-001_e20787` 有 `tts audio_start` 和 `tts audio_end`，`229440 bytes / 225 chunks`。
- 稍短回复也正常结束：`20260526T165702Z_cardputer-001_6957aa` 有 `tts audio_start` 和 `tts audio_end`，`157760 bytes / 155 chunks`。
- 打断测试日志符合预期：`20260526T165720Z_cardputer-001_71c722` 出现 `tts audio_start` 后被下一轮打断，没有继续发送 `audio_end`；随后新一轮 `20260526T165733Z_cardputer-001_0f4bc3` 正常回复并完整发完 `229440 bytes / 225 chunks`。
- 当前结论：服务端 paced 下发和打断保护从日志看已经成立；还需要用户反馈真机听感是否仍有无声、沙沙断裂或卡顿。若仍有问题，不要先加大 ring，优先打开/优化 `BIN/BUF/UND/DROP` 观测，或把服务端节奏从 30ms 微调到 32ms。

## 2026-05-27 TTS paced 32ms 微调

- 用户反馈：可以打断说话，但有时本该说话时只能听见小小沙沙声。
- 日志复查发现 `给我念一首古诗` 那轮只有 `tts audio_start` 没有 `audio_end`，随后有新一轮上行，判断为打断导致旧 TTS 停止；另发现 DeepSeek 空回复重试分支抛 `TypeError: Assignment to constant variable`，会导致算术题等场景走兜底。
- 已做服务端最小改动并部署：`sendChatTtsAudio()` 的 `pacedDelayMs` 从 `30` 调到 `32`，更贴近 16kHz / 512 samples 的真实播放节奏，降低偶发 ring 边界风险；`replyToChatWithDeepSeek()` 中首次 `result` 从 `const` 改为 `let`，修复空回复重试赋值错误。
- 本地 `node --check cloud-voice-server/server.js` 通过；远端备份 `/opt/cardputer-voice/server.js.bak-20260527-tts-paced-32ms-retry-fix`；上传后远端 `node --check /opt/cardputer-voice/server.js` 通过；`cardputer-voice.service` 重启后 `active`；`/health` 正常。
- 待真机复测：`给我念一首古诗`、`介绍一下你自己，三句话`。若仍出现 `SPEAKING` 但只有沙沙声，下一步先打开/优化 `BIN/BUF/UND/DROP` 观测，不要加大 ring。

## 2026-05-27 CHAT 交互与字符画

- 用户希望优化 C 键进入后的 CHAT 体验：不要空白和开头长测试音；颜色不要继续用绿色；支持“给我画一个小猪”时屏幕显示命令行风格字符画，但 TTS 不朗读字符画。
- 服务端已移除新连接后自动 `sendTestAudio(ws)` 的调用；保留测试音函数但不再用于正常 CHAT。连接成功后不再播放第 2 阶段测试长音。
- 固件 CHAT 默认主题色改为紫蓝色系，`chatShowDebugStats=false`，C 进入连接成功后直接显示 `READY`，并显示 `HOLD SPACE TO TALK`、`ESC EXIT   [ ] COLOR`。
- 固件文本页新增 ASCII art 检测与小字体逐行绘制：包含多行且 90% 以上 ASCII 时按字符画显示，不再把换行压成一行。
- 服务端新增 `displayText/speakText` 分离：`reply_text.text` 发给屏幕；TTS 使用 `speakText`。内置小猪字符画规则：用户说“画/字符画/ASCII + 小猪/猪/pig”时，屏幕显示 ASCII 小猪，TTS 只念“画好了。”。
- 本地 `node --check cloud-voice-server/server.js` 通过；固件编译通过并烧录 COM3：程序约 `1762812 bytes`，全局变量 `142760 bytes`（43%）；服务端远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-ui-ascii-art`，远端 `node --check` 通过，服务重启后 `active`，`/health` 正常。
- 待真机验证：C 进入不应再有开头测试长音；界面应为紫蓝色 READY；“给我画一个小猪”应显示字符画且只朗读短确认。

## 2026-05-27 CHAT UI 小字与播放卡顿微调

- 用户反馈：紫蓝色 READY 下方仍有绿色小字；小机子说话又变得一卡一卡。
- 固件已去掉 `drawChatStreamHome()` 里 READY 下方的 `HOLD SPACE TO TALK` / `ESC EXIT [ ] COLOR` 提示文本，避免蓝色主题下残留绿色小字。
- 固件播放路径微调：WebSocket 二进制音频帧到来时，不再每个包都调用 `speakerOn()`；仅当 `speakerOutputReady=false` 时初始化喇叭，减少播放期间重复触碰 Speaker 状态/音量造成的调度干扰。
- 已编译并烧录 COM3：程序约 `1762720 bytes`，全局变量 `142760 bytes`（43%）。待真机确认说话是否恢复流畅。

## 2026-05-27 CHAT 纯蓝主题与 30ms pacing

- 用户反馈：仍有绿色字，且语音仍一卡一卡。要求 CHAT 应用内部统一蓝色。
- 固件新增 CHAT 专用颜色：`CHAT_COL=0x04FF`、`CHAT_DIM=0x0252`，CHAT stream 区域的标题、状态、文本页标题、REC/调试文字都改为蓝色系；不再使用全局暗绿 `COL_DIM`。全局录音/播放/Wi-Fi 等界面不跟随此改动。
- 服务端 TTS `pacedDelayMs` 从 32ms 回到 30ms，避免 Node 定时器抖动导致实际发送慢于 16kHz/512-sample 播放节奏。
- 本地 `node --check cloud-voice-server/server.js` 通过；固件编译并烧录 COM3 成功：程序约 `1762720 bytes`，全局变量 `142760 bytes`（43%）；服务端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-blue-30ms`，远端 `node --check` 通过，服务重启后 `active`。
- 待真机确认：CHAT 内是否还有绿色；语音是否恢复顺畅。若仍卡顿，下一步短暂打开或自动显示 5 秒 `UND/DROP` 诊断，但默认界面保持纯蓝。

## 2026-05-27 TTS 抗抖余量回补

- 用户反馈：语音仍一卡一卡，并追问为什么之前解决后又出现。判断：后续修沙沙声、UI、pacing 调整让播放链路重新接近临界；服务端完整发完音频，问题更像固件播放缓冲余量被网络/Node 定时器抖动吃穿。
- 服务端最小改动：`sendChatTtsAudio()` 的 `prerollSamples` 从 `12288` 提到 `20000`，后续 `pacedDelayMs` 设为 `32`。目的：开头给固件约 1.25 秒音频余量，但后续按接近实时发送，避免长回复撑爆 `24576 samples` ring。
- 本地 `node --check` 通过；远端备份 `/opt/cardputer-voice/server.js.bak-20260527-tts-preroll-20000`；上传后远端 `node --check` 通过，确认线上 `prerollSamples=20000`、`pacedDelayMs=32`；服务重启后 `active`，`/health` 正常。
- 待真机复测：如果仍卡，下一步必须短暂打开/自动显示 `UND/DROP/BUF` 诊断，不再凭听感猜。

## 2026-05-27 TTS 卡顿诊断后修正

- 诊断数字：用户反馈卡顿时屏幕为 `BIN 240 / BUF 32 / UND 0 / DROP 0`。这说明 WebSocket 音频帧完整、无 ring 溢出、无播放饿空；`BUF 32` 是播完后不足一块的尾巴。问题不在服务端没发完，也不是缓冲欠载，而是 Speaker 播放调度过碎。
- 固件修正：`CHAT_PLAY_SAMPLES` 从 `256` 改回 `512`，恢复更稳的 32ms 播放块，减少 `Speaker.playRaw()` 调度碎片；`chatShowDebugStats` 改回 `false`。
- 已编译并烧录 COM3：程序约 `1762720 bytes`，全局变量约 `145832 bytes`（44%）。待真机确认卡顿是否消失。

## 2026-05-27 最新交接

这一文档最初写于开始重写前；现在阶段已经推进很多。新窗口应以本节和 `项目备忘.md` 末尾为准。

当前状态：

- `/chat/stream` WebSocket 实时 CHAT 主链路已跑通：长按空格说话，松开结束；服务端实时 ASR，DeepSeek 回复，CosyVoice TTS，固件边收边播。
- 旧 HTTP `/chat/upload` 仍保留为 fallback/debug，不要急着删。
- 普通录音/播放主线必须继续隔离保护。CHAT 的 PCM ring、播放任务、Mic/Speaker、WebSocket 都只允许在 `chatStreamScreen()` 内创建/占用，退出、断线、睡眠、打断时必须释放或停止。
- 最新固件已烧录到 COM3：程序约 `1762364 bytes`，全局变量约 `142760 bytes`（43%）。包含短 TTS 在 `audio_end` 后强制启动播放、播完回 `READY`、CHAT 内 `[`/`]` 换颜色、长按空格说话、打断播放。
- 最新服务端已部署到 `/opt/cardputer-voice/server.js` 并重启，`cardputer-voice.service` 为 `active`。包含小机子/大聪明人设、长期记忆、DeepSeek 空回复重试、非复读兜底、TTS 生成完成后再发 `tts_start`、TTS 下发节奏先预缓冲 `12288 samples` 再接近实时发送。
- 小机子人设：名字固定“小机子”，外号“大聪明”；人格是“机仆式随身逻辑伙伴”，接近贾维斯式冷静、精确、执行导向；绝不奉承；不自称“小智”或其他名字。

当前待验证：

- 用户刚反馈“有时候还是不出声，喇叭有沙沙声，可能是在说话”。服务端检查最近 TTS WAV 的 RMS/peak 正常，判断源音频不是噪声；已改服务端下发节奏防止 Cardputer 端 ring 溢出。
- 新窗口第一步建议让用户再测一个中等长度回复，例如“介绍一下你自己，三句话”，然后查日志是否有 `tts audio_start` 和 `tts audio_end`。
- 如果仍无声或沙沙，先做观测，不要盲目加大 ring：临时打开或优化 `BIN/BUF/UND/DROP`，同时查 `journalctl -u cardputer-voice.service --since '10 minutes ago' --no-pager | grep -E 'chat-stream|tts audio|tts failed|audio_end|reply'`。

当前下一步：

1. 真机复测 TTS paced 版本，确认中短回复、长回复、打断后新一轮回复是否都有声音且不卡。
2. 若 TTS 仍沙沙/无声，先加观测或调整服务端节奏到 512 samples / 32ms；不要动普通录音主线。
3. 人格/聪明度继续走服务端 prompt 和长期记忆，不需要刷机；若出现复读，先查是否 DeepSeek 空回复或 fallback。
4. 稳定后再考虑真正流式 TTS。当前仍是“完整生成 TTS WAV -> 抽 PCM -> 分片下发”。

## 当前结论

我们已经验证了 HTTP CHAT 能跑通：

- 小机器 `C` 键进入 CHAT。
- 录完整句话后上传 `/chat/upload`。
- 云端用 DashScope ASR 转文字。
- 云端用 DeepSeek 生成短回复。
- 云端尝试用 DashScope CosyVoice TTS 生成 WAV。
- 小机器可以显示回复，也已具备下载 `audioUrl` 并播放 WAV 的固件代码。

但这个链路是批处理：

```text
录完整段 -> 上传 -> ASR -> LLM -> TTS -> 下载 -> 播放
```

它天然延迟高，且 TTS 音质受小喇叭、采样率、音色、整段下载播放方式限制。用户现在想做的是实时随说随回，所以接下来不应继续深挖 HTTP CHAT，而应重写 CHAT 模块为实时流式架构。

## 已备份的稳定版本

当前可回退固件备份：

```text
build_backups/20260526_tts_24k_chat_reply/
```

其中：

- `toolbox.ino.merged.bin` 是完整 8MB 合并镜像，最适合直接烧录回退。
- `toolbox.ino.bin`、`toolbox.ino.bootloader.bin`、`toolbox.ino.partitions.bin` 是分开烧录用。
- `README.md` 记录了该版本能力和服务端要求。

当前固件也已烧录过 24k TTS 播放版，编译/上传成功。

## 不建议继续扩展的部分

这些是探索阶段产物，可以归档或停止扩展：

- 固件里的 HTTP CHAT 作为主体验。
- 本地 Codex 窗口自动输入脚本：
  - `tools/codex_window_dispatcher.ps1`
  - `tools/start_cardputer_codex_control.ps1`
  - `tools/stop_cardputer_codex_control.ps1`
- Heartbeat 自动唤醒方案已经删除，不要恢复。

原因：

- 窗口自动输入依赖焦点，可能打到别的窗口，不适合作为远程控制方案。
- HTTP CHAT 无法做到实时低延迟。
- 继续在 HTTP CHAT 上加功能会让 `toolbox.ino` 更乱。

## 应保留的稳定主线

不要动或尽量少动：

- 空格录音。
- 录音上传/flomo 链路。
- 录音列表与播放。
- 番茄钟。
- Wi-Fi/token/SD 配置。
- 服务器 `/upload`、普通录音 ASR、flomo、dashboard。

这些是当前项目可用价值，不要因为重写 CHAT 破坏。

## 建议的新结构

不要重写整个项目，只重写 CHAT 模块。

固件侧建议：

```text
toolbox/toolbox.ino
  录音/上传/播放/番茄钟保持稳定
  C 键入口改为新的实时 CHAT

experiments/chat_stream/
  README.md
  chat_stream_notes.md
  可选：最小独立实验草图
```

服务端建议：

```text
cloud-voice-server/server.js
  保留 /upload
  暂时保留 /chat/upload 作为 fallback/debug
  新增 /chat/stream WebSocket
```

如果 `server.js` 太大，可新建模块文件，但先不要大范围重构服务器主线。

## 推荐实施路线

### 第 1 阶段：最小 WebSocket 闭环

目标：证明小机器和服务器能建立长连接。

```text
小机器按 C
-> 连接 ws://cardputer.flye.cc/chat/stream
-> 服务器返回 hello/status JSON
-> 小机器屏幕显示 CONNECTED
```

先不接 ASR/TTS。

### 第 2 阶段：服务器向小机器流式发测试音频

目标：证明边收边播可行。

```text
服务器生成或读取一段 16k/24k PCM 测试音
-> WebSocket 二进制分片发送
-> 小机器 ring buffer 接收
-> Speaker 边收边播
```

这一步是实时语音体验的底座。

### 第 3 阶段：小机器边录边传

目标：证明麦克风音频分片稳定上行。

```text
小机器 Mic.record 小块 PCM
-> WebSocket 二进制帧发给服务器
-> 服务器保存为 WAV
-> 本地下载检查音质
```

先不接实时 ASR。

### 第 4 阶段：接实时 ASR

目标：服务器能边收边识别。

优先查 DashScope 实时语音识别 WebSocket 接口，避免继续用录音文件 ASR。

### 第 5 阶段：接 LLM + 流式 TTS

目标：接近实时语音助手体验。

```text
一句话结束/或 VAD 检测结束
-> LLM stream
-> TTS stream
-> 音频分片回传
-> 小机器边收边播
```

### 第 6 阶段：打断和状态机

目标：用户说话时可以打断正在播放的回复。

需要明确状态：

- listening
- uploading/streaming
- thinking
- speaking
- interrupted
- reconnecting

## C 键入口建议

用户已经同意 HTTP CHAT 不是刚需。下一窗口可以这样处理：

- `C` 默认进入新实时 CHAT。
- 旧 HTTP CHAT 逻辑可以先改名为 `chatHttpScreen()` 并不再从 launcher 暴露。
- 等实时 CHAT 稳定后，再删除旧 HTTP CHAT 固件代码。

建议不要第一步就删除服务器 `/chat/upload`，它仍可作为 debug/fallback。

## 当前服务端能力状态

`/health` 最近确认：

```text
uploadToken: true
chatReadToken: true
chatInboxPath: true
chatReply: true
chatTts: true
publicBaseUrl: true
asrFileToken: true
dashScope: true
deepSeek: true
flomo: true
```

TTS 当前配置：

```text
DASH_SCOPE_TTS_MODEL=cosyvoice-v3-flash
DASH_SCOPE_TTS_VOICE=longanyang
DASH_SCOPE_TTS_SAMPLE_RATE=24000
```

## 重要风险提醒

- 不要再依赖模拟键盘向 Codex 窗口输入；焦点会跑偏。
- 小机器端实时音频会和现有录音/播放共用 Mic/Speaker，状态切换要谨慎。
- `toolbox.ino` 已经很大，CHAT 重写要尽量局部化，不要顺手重构录音主线。
- 新实时功能先做实验入口，确认稳定后再覆盖 C 键体验。

## 下一窗口推荐第一条任务

建议直接对 Codex 说：

```text
请阅读 docs/NEXT_CHAT_STREAM_HANDOFF.md。我们要开始把 C 键 CHAT 重写成 WebSocket 实时语音，保留录音主线不动。先做第 1 阶段：服务端新增 /chat/stream WebSocket，固件新增 chatStreamScreen 最小连接闭环，只显示 CONNECTED，不接 ASR/TTS。
```

## 当前工作区提醒

工作区有很多已修改/未跟踪文件，其中不少是本轮探索产生的。不要随便 `git reset` 或删除。

优先关注：

- `toolbox/toolbox.ino`
- `cloud-voice-server/server.js`
- `cloud-voice-server/package.json`
- `cloud-voice-server/deploy/*.conf`
- `docs/NEXT_CHAT_STREAM_HANDOFF.md`
- `build_backups/20260526_tts_24k_chat_reply/`
## 2026-05-27 CHAT 稳定基线整理与小猪字符画本地验证

- 当前 CHAT 稳定基线先不继续扩大 ring，也不再动普通录音/播放主线；保持 CHAT 资源只在 `chatStreamScreen()` 内申请/释放。
- 固件侧核对结果：`CHAT_PLAY_SAMPLES=512`，`chatShowDebugStats=false`，CHAT 专用色为 `CHAT_COL=0x04FF`、`CHAT_DIM=0x0252`；`drawChatStreamHome()` 对非错误状态统一用 `chatAccentColor()`，不会因为调用处传 `COL_GREEN` 而显示绿色。
- 字符画显示链路核对结果：`reply_text.text` 进入 `drawChatStreamTextPage()`；多行且 90% 以上 ASCII 的内容走 `drawChatStreamAsciiText()`，使用 ASCII 小字体逐行绘制，不会被普通中文换行逻辑压成一行。
- 服务端小猪规则本地直测通过：输入 `给我画一个小猪` 命中 `asciiArtChatReply()`，返回多行 ASCII 到 `displayText`，`speakText` 只有 `画好了。`，不会把字符画交给 TTS 朗读。
- 本地验证：`node --check cloud-voice-server/server.js` 通过；`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762720 bytes`，全局变量 `145832 bytes`（44%）。本轮只编译验证，未重新烧录、未重新部署。
- 下一步真机验证口径：按 C 进入 CHAT 应直接看到蓝色系 `READY`；说 `给我画一个小猪` 后屏幕应显示多行命令行风格小猪字符画，喇叭只读短确认 `画好了。`；若 TTS 仍卡顿，先看 `BIN/BUF/UND/DROP` 或服务端 `tts audio_start/audio_end` 日志，不要凭听感继续加大 ring。
## 2026-05-27 小猪字符画换行/停留与复读保护

- 用户反馈：小猪有时由表情符号组成并排成一行，没有形成图案；即使画出来也约 1 秒被后续状态页盖掉；同时小机子显得很笨、只会复读。
- 固件根因 1：`extractJsonStringValue()` 把 JSON 字符串里的 `\n` 反转义成空格，导致服务端多行 `displayText` 到屏幕侧变成一行。已改为 `\n`/`\r` 转真实换行，`\t` 转空格。
- 固件根因 2：`reply_text` 画出回复页后，随后 `tts_start` 会立刻画 `SPEAKING`，播放结束又画 `READY`，所以字符画一闪而过。已新增 `chatTextPageVisible`：回复/ASR 文本页显示时，TTS 状态不覆盖页面；播放结束也不自动盖回 READY。开始新一轮说话、ASR/思考/保存等流程会正常清掉页面。
- 服务端修正：扩大小猪字符画规则触发词，`画/绘/字符画/ASCII/图案 + 猪/小猪/猪猪/pig` 强制走内置 ASCII 小猪，避免 DeepSeek 自由生成 emoji 图案；TTS 仍只读 `画好了。`。
- 服务端新增复读保护：如果 DeepSeek 的 `replyText/displayText/speakText` 与用户转写高度相似，就替换为非复读兜底，要求用户明确“要结论、解释，还是执行动作”，避免把 ASR 原话当回答。
- 本地验证：`node --check cloud-voice-server/server.js` 通过；规则直测 `给我画一个小猪`、`画只猪猪给我看` 均返回固定多行 ASCII；echo 检测直测为 true；`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762804 bytes`，全局变量 `145832 bytes`。
- 部署/刷机：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-art-page-echo-fix`；已上传 `/opt/cardputer-voice/server.js`，远端 `node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常；固件已编译并烧录 COM3，写入校验通过。
- 下一步真机验证：说“给我画一个小猪”应稳定显示多行 ASCII 小猪，并在 TTS 短确认结束后继续停留在图案页；再次长按空格开始新一轮时才切走。若仍复读，优先查服务端日志里实际 ASR 文本和 DeepSeek raw reply。
## 2026-05-27 CHAT 大脑降档与坏回复兜底修正

- 用户反馈：小机子仍显得很笨，一直复读，有时不管用户说什么。日志复查发现两个硬问题：线上 `/etc/cardputer-voice.env` 仍为 `DEEPSEEK_MODEL=deepseek-v4-flash`，不是之前文档期望的 pro；且 DeepSeek 偶发返回半截 JSON，例如 `{\n  "`，旧兜底会把这段半截内容当 `replyText/speakText` 下发和朗读。
- 服务端修正 1：线上环境变量已改为 `DEEPSEEK_MODEL=deepseek-v4-pro`，并重启服务。后续如果再次变笨，先查 `/etc/cardputer-voice.env` 是否被旧部署脚本或手工操作改回 flash。
- 服务端修正 2：新增 `isBadChatReplyText()`，空字符串、纯 `{}`/标点、半截 JSON、代码块开头等内容都不再当有效回复；会走非复读兜底，避免屏幕/TTS 出现 `{` 这种坏输出。
- 服务端修正 3：扩展内置小动物字符画规则：`画/绘/字符画/ASCII/图案/放/换 + 小猪/小狗/小鸡/小猫` 会直接返回固定 ASCII；如果只说“给我换一只”这类缺对象请求，会问“要哪种小动物？小猪、小狗、小鸡，选一个。”，不再交给模型乱猜或复读。
- 本地验证：`node --check cloud-voice-server/server.js` 通过；规则直测 `给我画一只小狗`、`给我放一只小鸡`、`给我画一只小猪` 均返回多行 ASCII；`给我换一只` 返回缺对象澄清；`isBadChatReplyText('{') === true`。
- 部署记录：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-brain-guard` 和 `/etc/cardputer-voice.env.bak-20260527-chat-brain-guard`；已上传服务端，远端 `node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常。本轮不需要重刷固件。
- 下一步真机验证：问一个需要推理的问题和一个小动物绘图问题，确认日志里的 `reply` 不再是原话或 `{`；若仍“答非所问”，先看日志中的 `realtime asr` 是否识别错了，而不是先改 prompt。
## 2026-05-27 CHAT 屏幕复读感修正与 Pro 输出预算

- 用户反馈：小猪基本没问题，但小机子仍“只会复读”。日志复查后判断：一部分复读感来自固件正常显示 `asr_text` 原文，也就是先把用户刚说的话铺到屏幕上；如果模型回复慢、被打断或用户只看屏幕，就会像小机子在复读。
- 固件修正：CHAT 正常模式收到 `asr_text` 后不再进入 `ASR TEXT` 大文本页显示用户原话，只短暂显示 `TEXT OK`；随后 `reply_start` 显示 `THINKING`，`reply_text` 才显示真正回复。`drawChatStreamAsrText()` 函数保留但当前主流程不调用。
- 服务端修正：DeepSeek CHAT `max_tokens` 从 `260` 提到 `700`，避免 `deepseek-v4-pro` 在 JSON 模式下因输出预算太小出现 `finishReason: length` / empty content。
- 本地验证：`node --check cloud-voice-server/server.js` 通过；`powershell -File tools/cardputer_build.ps1` 编译通过，程序 `1762784 bytes`，全局变量 `145832 bytes`。
- 部署/刷机：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-no-asr-echo-max-tokens`；服务端已上传，远端 `node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常，线上仍为 `DEEPSEEK_MODEL=deepseek-v4-pro`；固件已编译并烧录 COM3，写入校验通过。
- 下一步真机验证：说一句普通问题时，屏幕应显示 `TEXT OK -> THINKING -> REPLY`，不再先把用户原话作为大文本页展示；若仍听起来复读，查日志里 `reply` 是否等于 `realtime asr`，如果不等，问题可能是用户看到/听到的是 ASR 确认或旧轮次残留。
## 2026-05-27 服务器侧 CHAT 大脑 Probe 与上下文绘图修正

- 用户要求“先在服务器测试好再继续”。本轮没有刷固件，只改服务端并用线上 `/etc/cardputer-voice.env` 跑真实 DeepSeek probe。
- 新增 `CHAT_REPLY_PROBE=1` 隐藏测试入口：`set -a; . /etc/cardputer-voice.env; set +a; CHAT_REPLY_PROBE=1 node /opt/cardputer-voice/server.js` 会调用同一套 `replyToChatWithDeepSeek()` 和规则层，不启动 HTTP 服务。
- Probe 结果通过：`为什么你刚才一直像在复读` 能解释原因；`我想用两句话介绍一下这个小机器` 能给两句介绍；`算一下 17 乘以 23` 返回 `391`；确认模型为 `deepseek-v4-pro`。
- 规则层修正：如果用户说“这只小猪好丑”或“给我换一只小猪”，服务端直接返回第二版 ASCII 小猪，不再复读同一张图，也不把这类局部反馈丢给模型慢慢想。
- 提示词修正：要求模型使用最近上下文处理“换一个、太丑、继续”等短跟进；明显能执行的动作直接执行，只有对象真正缺失时才澄清。
- 部署记录：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-brain-probe-context-art`；上传服务端后先跑 `CHAT_REPLY_PROBE=1` 验证，再 `node --check`、重启 `cardputer-voice.service`，服务为 `active`，`/health` 正常。本轮不需重刷固件。
- 下一步真机建议：测试三句：`这只小猪好丑`、`为什么你刚才像在复读`、`算一下 17 乘以 23`。若真机仍显得傻，优先贴日志里的 `realtime asr` 和 `reply`，因为服务器 probe 已确认模型链路本身能正常回答。
## 2026-05-27 ASR 截断小猪请求默认补全

- 用户反馈：明明让画小猪，它仍问“要哪种小动物，小猪小狗小鸡”。最新日志确认 ASR 实际经常截成 `给我换一只可爱的小`、`给我画一只小`，末尾“猪”被吃掉；旧规则在缺动物时会澄清，所以显得很傻。
- 服务端修正：`asciiArtChatReply(userText, chatContext)` 现在接收最近上下文；绘图/换图请求里如果动物缺失、只听到“小”、或最近上下文是小猪，则默认补全为小猪并直接画第二版小猪，不再问“小猪小狗小鸡选一个”。
- 本地验证：`给我换一只可爱的小`、`给我画一只小`、`给我换一只` 在小猪上下文中均返回第二版 ASCII 小猪。
- 服务器验证：上传前备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-default-pig-on-truncated-art`；部署后用线上环境跑 `CHAT_REPLY_PROBE=1 CHAT_REPLY_PROBE_TEXTS='给我换一只可爱的小\n给我画一只小\n给我换一只...'`，三句均返回第二版小猪；随后 `node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常。本轮不需刷固件。
- 下一步真机验证：即使 ASR 只识别到 `给我换一只可爱的小`，也应直接画小猪，不再追问动物种类。
## 2026-05-27 CHAT 等待感与状态流简化

- 用户反馈：说完后要等一会儿，先出现“他的话/中间状态”，又等一会儿才出现真正回复，希望对话更顺畅。结论：真正零等待做不到，因为链路必须经过 ASR -> LLM -> TTS；但可以把 UI 从多段状态改成更直接的 `LISTEN -> THINKING -> REPLY`。
- 固件侧已改：`chatStopUplink()` 松开空格后直接显示 `THINKING`，不再显示 `SENDING`；收到 `uplink_saved/asr_start/asr_text` 不再覆盖主状态，只保留调试统计。这样新固件不会显示 `SAVED/ASR.../TEXT OK` 这些中间态。
- 本地固件编译通过：程序 `1762584 bytes`，全局变量 `145832 bytes`。但本机当前未检测到串口设备，`board list` 为空，刷 COM3 失败：`Could not open COM3, the port is busy or doesn't exist`。需要用户重新插拔/打开设备后再刷。
- 为兼容未刷的新固件，服务端也做了状态流简化：`uplink_end` 保存完音频后立即发送 `reply_start`，让旧固件尽快显示 `THINKING`；服务端不再发送 `uplink_saved` 和文件 ASR 的 `asr_text`，文件 ASR fallback 也只发 `reply_start`，避免旧固件被 `SAVED/ASR/TEXT OK` 覆盖。
- 服务端部署记录：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-fast-ui-status`；上传后 `CHAT_REPLY_PROBE=1` 验证 DeepSeek 正常；`node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常。
- 下一步：用户重新连接 Cardputer 后，运行 `powershell -File tools/cardputer_build.ps1 -Flash -Port COM3` 刷入固件侧 UI 简化。服务端改动已上线，即使未刷固件也应减少中间状态闪烁。
- 用户重新插上后 COM3 恢复；已执行 `powershell -File tools/cardputer_build.ps1 -Flash -Port COM3` 并刷入成功，写入各段 `Hash of data verified`，程序 `1762584 bytes`，全局变量 `145832 bytes`。现在固件侧也包含 `LISTEN -> THINKING -> REPLY` 状态流简化。
## 2026-05-27 THINKING 耗时快答层

- 用户反馈：`THINKING` 时间仍长。日志统计显示主要长在 DeepSeek Pro：部分问题 ASR 后到 `reply` 需要 6-13 秒，probe 中 Pro JSON 链路甚至可能 20 秒级；TTS 是回复文字出现后的语音阶段，不是 `THINKING` 的主因。
- 结论：真正零等待需要更大架构（流式 LLM/流式 TTS/边识别边生成），当前先做服务端快答层，常见简单意图不进 DeepSeek。
- 新增 `tryFastChatReply()`：算术（数字/十几/百以内常见中文数字的乘/加/减）、今天/日期/星期、自我介绍、为什么慢、`百年孤独` 简介直接由规则层秒答；绘图规则仍优先。
- 本地验证：`算一下十七乘以二十三 -> 17乘以23=391。`；`今天星期几 -> 今天是2026年5月27日，星期三。`；`你是谁`、`为什么 thinking 时间这么长`、`用100个字介绍百年孤独` 均返回规则答复。
- 服务器验证：远端备份 `/opt/cardputer-voice/server.js.bak-20260527-chat-fast-replies`；上传后串行跑 `CHAT_REPLY_PROBE=1`，上述样例均为 `provider: rules`，不进 DeepSeek；`node --check` 通过，`cardputer-voice.service` 重启后 `active`，`/health` 正常。本轮不需刷固件。
- 预期：简单问题的 `THINKING` 只剩 ASR 收尾和很短规则处理；复杂开放问题仍会等 DeepSeek Pro。
