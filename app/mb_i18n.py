#!/usr/bin/env python3
"""Monitor Box APP translations.

The APP is also useful without a desktop translation catalogue: keep messages in
the code, look them up by their Simplified Chinese source text, and fall back to
that source text when a translation is missing.  This is intentionally small and
has no external dependency, matching the offline target image.
"""

import re

LANGUAGES = ("zh_CN", "en")
LANGUAGE_LABELS = {"zh_CN": "简体中文", "en": "English"}

_language = "zh_CN"


def normalize_language(value):
    value = str(value or "").replace("-", "_")
    return "en" if value.lower().startswith("en") else "zh_CN"


def set_language(value):
    global _language
    _language = normalize_language(value)


def get_language():
    return _language


_EN = {
    # Window and common actions
    "监控盒子": "Monitor Box",
    "全屏": "Fullscreen",
    "关于": "About",
    "更多": "More",
    "设置": "Settings",
    "搜索设置 (Ctrl+F)": "Search settings (Ctrl+F)",
    "搜索设置项": "Search settings",
    "搜索结果": "Search results",
    "没有匹配的设置": "No matching settings",
    "保存": "Save",
    "取消": "Cancel",
    "删除": "Delete",
    "查看": "View",
    "重试": "Retry",
    "关闭": "Off",
    "开启": "On",
    "完成": "Finish",
    "反转": "Reverse",
    "推荐": "Suggest",
    "清空": "Clear",
    "放弃": "Discard",
    "继续编辑": "Keep editing",
    "知道了": "Got it",
    "稍后": "Later",
    "撤销": "Undo",
    "处理": "Review",

    # Header, idle page and status
    "正在连接…": "Connecting…",
    "事件 (F9)": "Events (F9)",
    "区域设置 (Ctrl+R)": "Regions (Ctrl+R)",
    "设置 (Ctrl+,)": "Settings (Ctrl+,)",
    "没有画面": "No signal",
    "正在等待视频源…": "Waiting for video…",
    "打开摄像头": "Turn camera on",
    "无信号": "No signal",
    "摄像头已关闭": "Camera is off",
    "待机 · 摄像头已关闭": "Standby · Camera off",
    "摄像头已打开": "Camera turned on",
    "摄像头已随 APP 打开": "Camera resumed with the APP",
    "摄像头已关闭, 盒子进入待机省电": "Camera turned off; the box is in power-saving standby",
    "关闭摄像头, 进入待机省电 (F8)": "Turn off the camera and enter standby (F8)",
    "打开摄像头 (F8)": "Turn on the camera (F8)",
    "盒子处于待机省电状态: 摄像头已释放, 不采集、不推理、不录像, 也不会产生告警。\nWeb 和事件查询照常可用。":
        "The box is in power-saving standby: the camera is released, capture, inference and recording are stopped, and no alerts are generated.\nWeb and event browsing remain available.",
    "摄像头已关闭 (待机)。检测服务启动后也会保持待机, 直到重新打开。":
        "The camera is off (standby). It will remain in standby after the detection service starts until you turn it back on.",
    "检测服务 mb-capture 暂时没有响应 (重启中?), 等它恢复后自动接回画面。":
        "The mb-capture detection service is not responding (possibly restarting). Video will reconnect automatically.",
    "mb-capture 在运行, 但摄像头断开了, 它正在重连。":
        "mb-capture is running, but the camera is disconnected and reconnecting.",
    "所有视频源都不可用。\n\n": "No video source is available.\n\n",
    "%s\n%d 秒后自动重试。": "%s\nRetrying automatically in %d seconds.",
    "检测服务已启动, 切换到它的视频流": "Detection service started; switching to its video stream",
    "画面中断, 正在重连…": "Video interrupted; reconnecting…",
    "%s: 画面中断": "%s: video interrupted",
    "视频流丢包, 正在重建画面…": "Video packet loss; rebuilding the stream…",
    "画面渲染停顿, 正在重建…": "Video rendering stalled; rebuilding…",
    "%s: 3 秒内没有画面": "%s: no video after 3 seconds",
    "%s · 连接中…": "%s · Connecting…",
    "连接失败, 换下一个源…": "Connection failed; trying the next source…",
    "无画面, 换下一个源…": "No signal; trying the next source…",
    "%s  ·  播放 %d fps": "%s  ·  Playing %d fps",
    "%s采集 %.1f fps  ·  AI %.1f ms\n时延 %.0f ms  ·  跟踪 %d":
        "%sCapture %.1f fps  ·  AI %.1f ms\nLatency %.0f ms  ·  Tracks %d",
    "节能 · ": "Eco · ",
    "摄像头断开, mb-capture 正在重连…": "Camera disconnected; mb-capture is reconnecting…",
    "mb-capture 没有在推 H.264": "mb-capture is not sending H.264",
    "活动热图: ": "Activity heatmap: ",
    "统计已关闭": "Statistics are off",
    "暂无活动数据，等待检测目标": "No activity yet; waiting for detected objects",
    "暂无活动数据，可在区域设置中开启「学习中」":
        "No activity yet; enable Learning in Regions to collect data",
    "当前是本地摄像头，热图需要检测服务的视频流":
        "The local camera is in use; the heatmap requires the detection service stream",
    "当前视频不是检测服务的视频流": "This video is not from the detection service",
    "视频流地址无效": "Invalid video stream address",
    "检测服务未推送 H.264 视频": "The detection service is not sending H.264 video",
    "当前视频端口与检测服务不一致": "The video port does not match the detection service",

    # Sources
    "自动": "Auto",
    "本地摄像头": "Local camera",
    "本地摄像头 720p": "Local camera 720p",
    "H.264 流": "H.264 stream",
    "Web 预览": "Web preview",

    # Events
    "暂无告警事件": "No events",
    "事件": "Events",
    "关闭事件侧栏": "Close event sidebar",
    "全选": "Select all",
    "全选当前列表": "Select all in this list",
    "较新的事件": "Newer events",
    "更早的事件": "Older events",
    "第 %d 页": "Page %d",
    "第 %d / %d 页": "Page %d of %d",
    "事件 (共 %d)": "Events (%d total)",
    "事件 (%d)": "Events (%d)",
    "删除 (%d)": "Delete (%d)",
    "批量删除事件?": "Delete selected events?",
    "将删除选中的 %d 条事件及其快照、录像。正在生成录像的事件会跳过。":
        "This deletes %d selected events, including snapshots and clips. Events still generating clips will be skipped.",
    "批量删除失败: %s": "Bulk delete failed: %s",
    "已删除 %d 条": "Deleted %d",
    "，%d 条录像生成中已跳过": "; skipped %d while clips are being generated",
    "，%d 条已不存在": "; %d no longer exist",
    "，%d 条删除失败": "; failed to delete %d",
    "录像中": "Recording",
    "冷却期合并 %d 条": "%d merged during cooldown",
    "没有更早的事件了": "No older events",
    "紧急": "Critical",
    "告警": "Warning",
    "信息": "Info",
    " (另有 %d 条)": " (%d more)",
    "正在加载快照…": "Loading snapshot…",
    "冷却期内另有 %d 条同类告警被合并": "%d similar alerts were merged during cooldown",
    "事件 ID %s": "Event ID %s",
    "播放录像": "Play clip",
    "播放事件录像": "Play the event clip",
    "录像生成中…": "Generating clip…",
    "录像失败": "Clip failed",
    "无录像": "No clip",
    "删除录像": "Delete clip",
    "删除事件": "Delete event",
    "快照": "Snapshot",
    "查看这条事件的快照": "View this event's snapshot",
    "只删除录像文件, 保留事件记录和快照": "Delete only the clip; keep the event and snapshot",
    "删除事件记录、快照和录像": "Delete the event, snapshot and clip",
    "这条事件没有快照": "This event has no snapshot",
    "快照加载失败: %s": "Failed to load snapshot: %s",
    "快照解码失败: %s": "Failed to decode snapshot: %s",
    "回放失败: %s": "Playback failed: %s",
    "无法回放: %s": "Cannot play clip: %s",
    "重播": "Replay",
    "播放中…": "Playing…",
    "删除录像?": "Delete clip?",
    "删除事件?": "Delete event?",
    "只删除录像文件, 事件记录和快照保留。":
        "Only the clip is deleted; the event and snapshot are kept.",
    "将删除这条事件的记录、快照和录像, 无法恢复。":
        "This permanently deletes the event, snapshot and clip.",
    "删除失败: %s": "Delete failed: %s",
    "未知错误": "Unknown error",
    "事件已删除": "Event deleted",
    "录像已删除": "Clip deleted",
    "无效响应": "Invalid response",

    # Feature names and descriptions
    "物体检测": "Object detection",
    "检测并显示 COCO 80 类物体; 各功能只按自己的目标类别告警":
        "Detect and display all 80 COCO classes; each feature alerts only for its configured classes",
    "入侵检测": "Intrusion detection",
    "有人进入警戒区就告警": "Alert when a person enters a protected zone",
    "跌倒检测": "Fall detection",
    "人躺倒姿势持续 1 秒": "Detect a person remaining in a fallen pose",
    "人数统计": "People counting",
    "按门口画线累计进 / 出": "Count entries and exits across a line",
    "火焰烟雾": "Fire and smoke",
    "专用模型, 需要连续确认": "Dedicated model with multi-frame confirmation",
    "电子围栏": "Electronic fence",
    "跨过围栏线的目标告警": "Alert when a target crosses the fence line",
    "人员滞留": "Loitering",
    "同一目标在警戒区停留超过设定时间后告警":
        "Alert when the same target stays beyond the configured time",
    "包裹检测": "Package detection",
    "静止没人看管的包": "Detect unattended stationary packages",
    "定位系统": "Location mapping",
    "地面定位": "Ground mapping",
    "人员/车辆/宠物的地面坐标、轨迹、区域越界与鸟瞰图":
        "Ground coordinates, trails, zone violations and a bird's-eye map for people, vehicles and pets",

    # Settings
    "常规": "General",
    "检测与音频": "Detection & audio",
    "视频与显示": "Video & display",
    "运行与关于": "Runtime & about",
    "应用设置": "Application",
    "界面语言": "Interface language",
    "界面语言已保存": "Interface language saved",
    "选择 APP 的显示语言, 保存后立即生效。":
        "Choose the APP display language. Changes apply immediately.",
    "检测能力": "Detection",
    "功能总开关": "Feature master switches",
    "每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。mb-capture 正在运行, 开关立即生效。":
        "Each feature has one master switch; regions and crossing lines only decide where it applies. mb-capture is running, so changes apply immediately.",
    "每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。mb-capture 没在运行: 开关写入配置文件, 它下次启动时生效。":
        "Each feature has one master switch; regions and crossing lines only decide where it applies. mb-capture is not running, so changes are saved for its next start.",
    "每个功能只有一个总开关；区域和进出线只决定功能在哪些位置生效。":
        "Each feature has one master switch; regions and crossing lines only decide where it applies.",
    "mb-capture 正在运行, 开关立即生效。":
        "mb-capture is running; switches apply immediately.",
    "mb-capture 没在运行: 开关写入配置文件, 它下次启动时生效。":
        "mb-capture is not running; switches are saved and apply on its next start.",
    "滞留告警时间（秒）": "Loitering alert time (seconds)",
    "1–86400 秒，默认 60 秒；确认告警后播放警笛（需启用扬声器）。":
        "1–86400 seconds; default 60. Plays the siren after confirmation when speaker output is enabled.",
    "包裹静止告警时间（秒）": "Stationary package time (seconds)",
    "1–86400 秒，默认 20 秒；静止满设定时间后触发告警录像。":
        "1–86400 seconds; default 20. Triggers an alert clip after the package remains stationary.",
    "越线计数清零周期（小时）": "Line count reset interval (hours)",
    "0 = 不自动清零；设成 N 则每 N 小时清零一次，按本地时间整点对齐。历史累计仍按小时写入 counts.jsonl，不受影响。":
        "0 = never reset automatically. N resets every N hours aligned to local wall-clock time. Hourly history in counts.jsonl is unaffected.",
    "当前人数统计": "Current people counts",
    "清零所有计数线的进入和离开当前值；已写入 counts.jsonl 的历史记录不受影响。":
        "Reset the current in and out values for every counting line; history already written to counts.jsonl is unaffected.",
    "立即清零": "Reset now",
    "清零所有人数统计？": "Reset all people counts?",
    "所有计数线的进入和离开当前值会归零；已写入 counts.jsonl 的历史记录不受影响。":
        "The current in and out values of every counting line will be reset to zero; history already written to counts.jsonl is unaffected.",
    "人数统计已清零": "People counts reset",
    "事件音频": "Event audio",
    "仅控制扬声器播放，不影响检测、事件记录和录像。告警使用普通警笛，紧急使用急促警笛；需开启对应检测功能。":
        "Controls speaker playback only; detection, event history and recording are unaffected. Enable the matching detection feature first.",
    "电子围栏跨越": "Fence crossing",
    "开启后播放该类事件音频": "Play audio for this event type",
    "电源与功耗": "Power",
    "关闭 APP 不会停止检测服务; 不需要监控时可让盒子待机, 摄像头、NPU 推理和视频编码全部停下。":
        "Closing the APP does not stop detection. Use standby when monitoring is not needed to release the camera and stop NPU inference and video encoding.",
    "工作模式": "Operating mode",
    "正常: 全帧率 · 节能: 降低采集/推理帧率, 告警照常 · 待机: 关闭摄像头, 不告警":
        "Normal: full frame rate · Eco: lower capture/inference rate; alerts continue · Standby: camera off, no alerts",
    "正常": "Normal",
    "节能": "Eco",
    "待机 (关闭摄像头)": "Standby (camera off)",
    "摄像头随 APP 开关": "Camera follows APP",
    "退出 APP 时让盒子待机, 再次打开 APP 时恢复。关闭此项则 APP 退出后盒子继续后台检测告警。":
        "Enter standby when the APP closes and resume when it opens. When off, detection continues in the background.",
    "视频来源": "Video source",
    "自动: mb-capture 在跑就收它的流 (优先 H.264), 否则直连摄像头。":
        "Auto: use mb-capture's stream when available (prefer H.264), otherwise open the camera directly.",
    "来源": "Source",
    "画面显示": "On-screen display",
    "只调整信息显示, 不影响检测、跟踪和告警。":
        "Changes displayed information only; detection, tracking and alerts are unaffected.",
    "视频内性能指标": "In-video performance metrics",
    "显示 FRAME / DET / TRK / AI 信息条; 同步影响 APP、Web 及后续录像和快照。":
        "Show FRAME / DET / TRK / AI metrics in the APP, Web view, recordings and snapshots.",
    "标题下方视频状态": "Video status below title",
    "仅在 APP 标题下方单行显示视频来源、播放/采集帧率、AI 耗时和时延。":
        "Show source, playback/capture FPS, AI time and latency in one line below the APP title.",
    "隐藏区域与进出线": "Hide regions and crossing lines",
    "不显示区域填充、区域边框和进出线；检测、计数和告警仍照常运行。":
        "Hide region fills, region outlines and crossing lines; detection, counting and alerts continue normally.",
    "运行状态": "Runtime status",
    "当前视频源": "Current source",
    "检测服务 mb-capture": "Detection service (mb-capture)",
    "运行中 · 采集 %.1f fps · AI %.0f ms · 解码 %.0f ms · 时延 %.0f ms":
        "Running · capture %.1f fps · AI %.0f ms · decode %.0f ms · latency %.0f ms",
    "摄像头": "Camera",
    "已关闭 (待机省电)": "Off (power-saving standby)",
    "%s · %s · %dx%d %s · 重连 %d 次":
        "%s · %s · %dx%d %s · %d reconnects",
    "已连接": "Connected",
    "断开": "Disconnected",
    "H.264 推流": "H.264 output",
    "未运行": "Not running",
    "摄像头节点": "Camera device",
    "  (APP 正在使用)": "  (in use by APP)",
    "配置文件": "Configuration file",
    "未找到, 使用内置默认值": "Not found; using built-in defaults",
    "版本": "Version",
    "正在保存配置, 请稍后打开设置": "Saving configuration; open Settings again shortly",
    "正在保存配置, 请稍后再打开区域设置":
        "Saving configuration; open Regions again shortly",
    "读取检测服务配置失败: %s": "Failed to read detection service configuration: %s",
    "配置被拒绝: %s": "Configuration rejected: %s",
    "检测服务连接失败, 未保存: %s": "Detection service connection failed; not saved: %s",
    "检测服务连接失败, 未确认保存: %s":
        "Detection service connection failed; save not confirmed: %s",
    "已保存到文件; 检测服务未确认, 启动后生效":
        "Saved to file; the detection service will apply it on next start",
    "已保存到文件; 检测服务启动后生效":
        "Saved to file; applies when the detection service starts",
    "已保存, 需重启 mb-capture 生效: %s": "Saved; restart mb-capture to apply: %s",
    "配置已保存": "Configuration saved",
    "检测功能设置已保存": "Detection feature settings saved",
    "显示设置已保存": "Display settings saved",
    "热图显示设置已保存": "Heatmap display setting saved",
    "区域已保存": "Regions saved",
    "区域最多只能有 %d 个": "At most %d regions are allowed",
    "区域最多只能有 %d 个; 请先删除不用的区域":
        "At most %d regions are allowed; delete an unused region first",
    "; 区域已达 %d 个上限, 未添加 %d 个区域建议":
        "; the %d-region limit was reached, so %d suggested regions were not added",
    "正在保存其它配置, 请稍后再试": "Another configuration is being saved; try again shortly",
    "滞留告警时间已设为 %d 秒": "Loitering alert time set to %d seconds",
    "包裹静止告警时间已设为 %d 秒": "Stationary package time set to %d seconds",
    "越线计数%s": "Line counting: %s",
    "不自动清零": "no automatic reset",
    "每 %d 小时清零一次": "reset every %d hours",
    "%s已%s": "%s %s",
    "事件音频已%s": "Event audio %s",
    "工作模式已切换为「%s」": "Operating mode changed to \"%s\"",
    "摄像头将随 APP 开关": "Camera will follow the APP",
    "已关闭「摄像头随 APP 开关」, 退出 APP 后继续后台检测":
        "Camera-follows-APP is off; detection continues in the background after the APP closes",
    "USB(UVC) 摄像头 + 板载 NPU 推理。\n画面和告警来自 mb-capture; 检测能力在「设置」里开。":
        "USB (UVC) camera with on-device NPU inference.\nVideo and alerts come from mb-capture; enable detection features in Settings.",

    # Camera alignment notices
    "摄像头画面有偏移, 区域已自动跟随": "Camera view shifted; regions followed automatically",
    "区域已自动跟随画面": "Regions followed the camera view",
    "检测到摄像头画面整体偏移 (%s), 区域、进出线和地面标定已按同样的偏移调整, 检测照常进行。\n\n可以打开区域设置核对一下位置; 如果其实是有意调整了摄像头、想让区域保持原来在画面上的位置, 选「撤销」。":
        "The camera view shifted (%s). Regions, crossing lines and ground calibration were adjusted by the same amount; detection continues.\n\nOpen Regions to verify the result. If the camera was moved intentionally and you want the previous on-screen placement, choose Undo.",
    "核对区域": "Check regions",
    "摄像头位置变化较大, 区域需要确认": "Camera moved significantly; regions need review",
    "摄像头位置变化较大": "Camera moved significantly",
    "画面偏移 (%s) 超出了自动跟随的范围, 区域保持原样, 和画面可能已经对不上。\n\n建议打开区域设置按当前画面调整后保存; 如果区域仍然合适, 选「以当前画面为准」。":
        "The view shift (%s) is too large for automatic adjustment. Regions were left unchanged and may no longer align.\n\nOpen Regions and adjust them for the current view. If they still align, choose Use current view as reference.",
    "以当前画面为准": "Use current view",
    "调整区域": "Adjust regions",
    "画面长时间对不上参考画面, 摄像头可能被移动或遮挡":
        "View has not matched the reference; the camera may have moved or been obstructed",
    "画面和参考画面对不上": "View does not match the reference",
    "最近一段时间的画面和保存区域时的画面差别很大, 可能是摄像头被转动、遮挡、镜头脏了, 也可能只是光线/场景变化大。\n\n如果画面正常、区域位置也正确, 选「画面正常」, 以后会把这种画面也当作参考; 否则请检查摄像头, 或打开区域设置调整。":
        "Recent video differs greatly from the view used to save the regions. The camera may have moved, become obstructed or dirty, or lighting/scene conditions may have changed.\n\nIf the view and regions are correct, choose View is normal so this view is accepted as another reference. Otherwise check the camera or adjust Regions.",
    "画面正常": "View is normal",
    "已撤销自动跟随, 区域按原来的位置使用":
        "Automatic adjustment undone; regions use their previous placement",
    "已以当前画面为准": "Current view saved as reference",
    "已记下当前画面, 以后按它对齐": "Current view recorded for future alignment",
    "操作失败: %s": "Operation failed: %s",

    # Event descriptions
    "进入": "Entered",
    "离开": "Exited",
    "越线": "Crossed line",
    "滞留": "Loitering",
    "超长滞留": "Extended loitering",
    "滞留结束": "Loitering ended",
    "遗留": "Left behind",
    "取走": "Removed",
    "跌倒": "Fell",
    "位置": "Position",
    "方向": "Direction",

    # Region editor
    "区域设置": "Regions",
    "画地面标定": "Draw ground calibration",
    "定位必须先画地面标定: 依次点击地面上已知矩形的四个角, 再填写实际宽度和纵深":
        "Location mapping requires ground calibration first: click the four corners of a known ground rectangle, then enter its actual width and depth",
    "重新抓取画面和热图": "Refresh video and heatmap",
    "保存区域和显示设置": "Save regions and display settings",
    "画多点区域": "Draw region",
    "画进出线": "Draw crossing line",
    "区域与进出线": "Regions and crossing lines",
    "批量管理": "Batch manage",
    "勾选多项后一次删除": "Select multiple items to delete at once",
    "已选择 %d 项": "%d items selected",
    "选择此项用于批量删除": "Select this item for batch deletion",
    "删除选中": "Delete selected",
    "删除选中 (%d)": "Delete selected (%d)",
    "批量删除区域?": "Delete selected regions?",
    "将删除选中的 %d 项区域、进出线或地面区域；仍需点顶部「保存」才会写入检测服务。":
        "Delete the selected %d regions, crossing lines or ground zones? Click Save at the top to apply the deletion.",
    "已删除 %d 项，启用在这些项上的功能一并取消":
        "Deleted %d items; features enabled on them were unbound",
    "同时创建地面区域": "Also create a ground zone",
    "画地面标定时用同一范围创建地面区域; 告警默认关闭, 可稍后设置":
        "Create a ground zone from the same footprint while drawing calibration; alarms stay off until configured",
    "用标定范围创建地面区域": "Create ground zone from calibration",
    "%s 地面区域": "%s ground zone",
    "可选; 无需再画一遍, 告警默认关闭":
        "Optional; reuse the calibration footprint without drawing it again. Alarms are off by default",
    "无需再画一遍; 告警默认关闭, 可稍后设置进入和停留告警":
        "No need to draw it again; entry and dwell alarms stay off until configured",
    "创建": "Create",
    "地面区域（可选）": "Ground zones (optional)",
    "完成地面标定即可开启定位；这里用于额外设置越界和停留规则。可在鸟瞰图上按米画, 或用上方按钮直接复用标定范围。每个区域可单独启用、设置越界进入告警和停留超时告警；这些都受上方功能总开关约束。":
        "Ground calibration alone is enough to enable location mapping. Ground zones add entry and dwell rules. Draw them in meters on the bird's-eye view, or reuse the calibration footprint with the button above. Each zone can be enabled separately and configured for entry and dwell alarms under the feature master switch.",
    "打开区域设置并直接开始画地面标定":
        "Open Regions and start drawing ground calibration",
    "已开启 · 可直接在这里关闭；地面区域规则正在生效":
        "On · turn it off here; ground-zone rules are active",
    "未开启 · 完成地面标定后可直接在这里打开":
        "Off · complete ground calibration, then turn it on here",
    "先完成地面标定, 才能用标定范围创建地面区域":
        "Complete ground calibration before creating a ground zone from its footprint",
    "已用标定范围创建地面区域; 告警默认关闭, 可继续设置后保存":
        "Ground zone created from the calibration footprint; alarms are off until configured",
    " 同时已用标定范围创建告警默认关闭的地面区域。":
        " A ground zone with alarms off was also created from the calibration footprint.",
    "最多可创建 12 个区域 (含地面标定和忽略区)。点击一项展开设置和功能绑定; "
    "支持多点多边形, 不规则区域建议 8～12 点。实线 = 监控画面显示; 虚线 = 不显示。":
        "Up to 12 regions can be created, including ground calibration and ignore regions. "
        "Click an item to expand its settings and feature bindings. Supports multi-point polygons; "
        "8–12 points suit irregular regions. Solid = shown on the monitoring view; dashed = hidden.",
    "支持多点多边形, 不规则区域建议 8～12 点。实线 = 监控画面显示; 虚线 = 不显示。":
        "Supports multi-point polygons; 8–12 points suit irregular regions. Solid = shown on the monitoring view; dashed = hidden.",
    "还没有区域, 点画面上方「画多点区域」开始":
        "No regions yet. Use Draw region above the image to start.",
    "选中项": "Selected item",
    "名称、在这里启用和删除操作都作用于当前选中项；修改后点顶部「保存」才会生效。":
        "Name, feature switches and delete all apply to the selected item. Click Save at the top to apply changes.",
    "名称、区域绑定和删除操作都作用于当前选中项；修改后点顶部「保存」才会生效。":
        "Name, region bindings and delete all apply to the selected item. Click Save at the top to apply changes.",
    "名称": "Name",
    "用途": "Role",
    "顶点": "Vertices",
    "顶点: %d 个": "Vertices: %d",
    "添加顶点": "Add vertex",
    "实际宽度 (米)": "Actual width (m)",
    "近处两个角之间的距离": "Distance between the two near corners",
    "实际纵深 (米)": "Actual depth (m)",
    "近边到远边的距离": "Distance from the near edge to the far edge",
    "进出方向": "Crossing direction",
    "画面上箭头指向的一侧算「进」": "The arrow points toward the entry side",
    "使用它的功能": "Used by",
    "删除选中项后仍需点顶部「保存」才会写入检测服务。":
        "After deleting an item, click Save above to write it to the detection service.",
    "删除选中项": "Delete selected item",
    "区域或进出线会从当前配置中移除": "Removes the region or crossing line from the configuration",
    "区域或进出线会从当前配置中移除；仍需点顶部「保存」才会写入检测服务。":
        "Removes the region or crossing line from the current configuration; click Save at the top to write it to the detection service.",
    "功能概览": "Feature overview",
    "各功能启用在哪些区域 / 进出线上。入侵 / 围栏 / 滞留建议用「自家区域」; 定位要用「地面标定」。":
        "Where each feature is enabled. Use Private area for intrusion, fence and loitering; "
        "use Ground calibration for location mapping.",
    "总开关在「设置 > 功能总开关」；这里只显示各功能绑定在哪些区域 / 进出线上。入侵 / 围栏 / 滞留建议用「自家区域」; 地面定位要用「地面标定」。":
        "Master switches are in Settings > Feature master switches; this page only shows region and line bindings. Use Private area for intrusion, fence and loitering, and Ground calibration for ground mapping.",
    "在这里启用": "Enable here",
    "在此处启用": "Enable here",
    "同一功能可以在多个区域 / 进出线上启用, 每处可以单独调参数。":
        "A feature can be enabled on several regions or lines, each with its own settings.",
    "这里只控制当前区域 / 进出线；功能总开关在「设置 > 功能总开关」中，同一功能可在多处绑定并分别调参数。":
        "These switches only apply to the selected region or line. Master switches are in Settings > Feature master switches; a feature can be bound in several places with separate settings.",
    "这里只控制当前区域 / 进出线；功能总开关可在「设置 > 功能总开关」或「鸟瞰图 / 定位」中修改，同一功能可在多处绑定并分别调参数。":
        "These switches only apply to the selected region or line. Master switches are available in Settings > Feature master switches or in Bird's-eye view / Location; a feature can be bound in several places with separate settings.",
    "还没有在任何%s上启用": "Not enabled on any %s yet",
    "喇叭警告": "Speaker warning",
    "关掉后这里只报警录像, 不喊话": "When off, alarms and recordings here stay silent",
    "沿用功能默认值": "Uses the feature default",
    "仅此处": "This item only",
    "持续多久才报 (秒)": "Report after (seconds)",
    "滞留多久才报 (秒)": "Loitering threshold (seconds)",
    "%s只能使用一个区域, 已从「%s」取消": "%s can use only one region; removed from “%s”",
    "推荐理由": "Why suggested",
    "推荐理由: ": "Why suggested: ",
    "活动热图": "Activity heatmap",
    "统计人和车在画面里经过的位置、走向和出现/消失的地方。学习一段时间 (最好覆盖白天和晚上) 后, 可以按热图推荐马路、人行道、通道、门口和进出线。":
        "Collects where people and vehicles pass, which way they move and where they appear or "
        "disappear. After learning through day and night, suggests roads, sidewalks, paths, "
        "entrances and crossing lines.",
    "在画面上显示热图": "Show heatmap on video",
    "青色 = 人, 橙色 = 车; 保存后也显示在主画面":
        "Cyan = people, orange = vehicles; also shown in the main view after saving",
    "学习中": "Learning",
    "检测功能全关时也运行检测模型, 只为统计热图 (占用 NPU)":
        "Run detection for heatmap statistics even when all features are off (uses NPU)",
    "统计": "Statistics",
    "读取中…": "Loading…",
    "按热图推荐区域和进出线": "Suggest regions and lines from heatmap",
    "马路、通道、门口和进出线; 只是建议, 保存前请检查和调整":
        "Roads, paths, entrances and crossing lines; suggestions only, review before saving",
    "清空热图": "Clear heatmap",
    "摄像头挪动过位置后需要重新统计":
        "Restart statistics after moving the camera",
    "没有画面: 检测服务未运行、摄像头已关闭或处于待机。仍可编辑, 但看不到参照。":
        "No video: the detection service is not running, or the camera is off/standby. You can still edit without a visual reference.",
    "画面解码失败: %s": "Failed to decode video: %s",
    "正在抓取画面…": "Capturing video…",
    "读取失败: %s": "Load failed: %s",
    "自 %s 起: 人 %d 次 · 车 %d 次%s":
        "Since %s: %d people · %d vehicles%s",
    " (统计已关闭)": " (statistics off)",
    "区域至少要 3 个点": "A region needs at least 3 points",
    "边交叉了, 请按顺时针或逆时针依次点击":
        "Edges intersect; click points consistently clockwise or counter-clockwise",
    "区域至少 3 个点; 地面标定必须 4 个点":
        "A region needs at least 3 points; ground calibration needs 4",
    "请点击两个顶点之间的边缘, 不要点在已有顶点上":
        "Click the edge between two vertices, away from existing vertices",
    "请点击选中区域的边缘添加顶点": "Click the selected region's edge to add a vertex",
    "加点后边会交叉, 请换一个位置":
        "Adding a vertex would cross an edge; choose another position",
    "画多点区域: 沿边界依次点击 (已 %d 个点, 不规则区域建议 8～12 点)。点回第一个点、双击或按回车完成; 右键/退格撤销上一个点; Esc 取消。":
        "Draw region: click along the boundary (%d points; 8–12 suit irregular regions). Click the first point, double-click or press Enter to finish; right-click/Backspace removes the last point; Esc cancels.",
    "画地面标定: 依次点击地面上已知矩形的四个角 (已 %d / 4 个点)。先近左、近右、远右、远左更容易理解; 点完第 4 点会自动完成, 再填写实际宽度和纵深 (米)。右键/退格撤销; Esc 取消。":
        "Draw ground calibration: click the four corners of a known ground rectangle (%d / 4). Near-left, near-right, far-right, far-left is easiest to follow; it finishes after the fourth point, then enter the actual width and depth in meters. Right-click/Backspace undoes; Esc cancels.",
    "画进出线: 点击起点和终点。箭头指向的一侧算「进」, 画完可在右侧反转。Esc 取消。":
        "Draw crossing line: click start and end. The arrow points toward Entry; reverse it from the sidebar. Esc cancels.",
    "添加顶点: 单击选中区域的边缘加点, 可连续添加; 再点「添加顶点」、右键或 Esc 退出。":
        "Add vertex: click the selected region's edge to add points; click Add vertex again, right-click or press Esc to stop.",
    "地面标定固定四点, 对应实际矩形的四个角; 拖动白点调整, 不支持增删顶点。":
        "Ground calibration has four fixed points for the rectangle corners. Drag the white handles; adding or removing vertices is disabled.",
    "已选区域: %d 个顶点。拖动白点改形状, 拖动内部平移; 方向键微调位置 (Shift 加速); 点「添加顶点」再单击边缘, 或双击边缘加点; 右键白点删点。":
        "Selected region: %d vertices. Drag handles to reshape and the interior to move. Arrow keys nudge it (Shift for larger steps). Use Add vertex then click an edge, or double-click an edge; right-click a handle to remove it.",
    "拖动端点调整进出线, 拖动内部整体平移, 方向键微调位置; Delete 删除选中项。":
        "Drag endpoints to adjust the crossing line, drag its interior to move it, or use the arrow keys to nudge it; press Delete to remove it.",
    "点选区域或线进行修改; 点「画多点区域」沿边界绘制, 不规则区域建议 8～12 点。":
        "Select a region or line to edit, or use Draw region along the boundary; 8–12 points suit irregular regions.",
    "近左": "Near left",
    "近右": "Near right",
    "远右": "Far right",
    "远左": "Far left",
    "进": "In",
    " (不显示)": " (hidden)",
    "进出线 · ": "Crossing line · ",
    "所有功能都跳过这里": "All features ignore this area",
    "未开启": "off",
    "%s (未开启)": "%s (off)",
    "%s (功能总开关未开启)": "%s (feature master switch off)",
    "监控画面不显示": "hidden from monitoring view",
    "未使用": "Unused",
    "不使用": "None",
    "区域": "Region",
    "进出线": "Crossing line",
    "选中: ": "Selected: ",
    "没有功能使用": "No feature uses this item",
    "地面标定固定四点, 对应实际矩形的四个角":
        "Ground calibration has four fixed points for the rectangle corners",
    "不规则区域建议 8～12 点, 可继续加点或右键白点删点":
        "Irregular regions suit 8–12 points; add more or right-click a handle to remove one",
    "功能已开启": "Feature on",
    "功能未开启 (在「设置」里打开)": "Feature off (enable in Settings)",
    "功能在此处已开启": "Feature on here",
    "此处已绑定，但功能总开关未开启": "Bound here, but the feature's main switch is off",
    "功能已开启，但此处未启用": "Feature is on, but not enabled here",
    "此处已启用，功能总开关也已开启": "Enabled here; the feature master switch is also on",
    "此处已绑定；功能总开关未开启，仍不会运行":
        "Bound here, but the feature master switch is off, so it will not run",
    "功能总开关已开启；此处尚未绑定": "Feature master switch is on; not bound here",
    "功能总开关未开启；此处也未绑定": "Feature master switch is off; not bound here",
    "总开关已开启": "Main switch on",
    "总开关未开启": "Main switch off",
    "地面标定必须正好 4 个点 (地面上一个矩形的 4 个角), 请重新画":
        "Ground calibration must have exactly 4 points (the corners of a ground rectangle); redraw it",
    "地面标定必须依次点 4 个角": "Ground calibration requires four corners",
    "地面标定必须正好 4 个点": "Ground calibration must have exactly 4 points",
    "地面标定已创建并绑定地面定位。请在右侧填写这块地面的实际宽度和纵深, 然后点顶部「保存」":
        "Ground calibration created and bound to ground mapping. Enter its actual width and depth on the right, then click Save at the top",
    "地面标定已创建并绑定地面定位。请在右侧填写实际宽度和纵深, 然后点顶部「保存」":
        "Ground calibration created and bound to ground mapping. Enter its actual width and depth on the right, then click Save at the top",
    "地面标定已创建并绑定地面定位。请填写实际宽度和纵深并保存, 再到「设置 > 地面定位与鸟瞰」打开总开关":
        "Ground calibration created and bound to ground mapping. Enter its actual width and depth, save, then turn on the master switch in Settings > Ground mapping and bird's-eye view",
    "地面标定已创建并绑定地面定位。请填写实际宽度和纵深并保存, 再到「鸟瞰图 / 定位」或「设置 > 地面定位与鸟瞰」打开总开关":
        "Ground calibration created and bound to ground mapping. Enter and save its actual width and depth, then turn on the master switch in Bird's-eye view / Location or Settings > Ground mapping and bird's-eye view",
    "%s不能使用这种区域, 已取消引用":
        "%s cannot use this region type; its assignment was removed",
    "已删除; 启用在它上面的%s一并取消": "Deleted; %s on it were removed too",
    "热图学习已%s": "Heatmap learning %s",
    "还没有热图数据": "No heatmap data yet",
    "清空活动热图?": "Clear activity heatmap?",
    "已统计的人/车活动会全部清零, 从现在开始重新统计。":
        "All collected person and vehicle activity will be cleared and collection will restart.",
    "清空失败: %s": "Failed to clear: %s",
    "热图已清空": "Heatmap cleared",
    "先完成或取消正在画的形状": "Finish or cancel the current shape first",
    "请先重启 APP，再保存热图显示设置。":
        "Restart the APP before saving the heatmap display setting.",
    " (还有 %d 处)": " (%d more)",
    "放弃修改?": "Discard changes?",
    "区域、线或热图显示设置的改动还没有保存。":
        "Region, line or heatmap display changes have not been saved.",
    "摄像头画面有偏移, 区域已按自动跟随后的位置显示; 保存后以当前画面为准":
        "The camera view shifted; regions are shown at the automatically adjusted positions. Saving uses the current view as the reference.",
    "沿边界连续点击添加顶点, 不规则区域建议 8～12 点, 不限于四点":
        "Click along the boundary to add vertices; 8–12 points suit irregular regions and the shape is not limited to four points.",
    "开启后单击选中区域的边缘加点, 可连续添加; Esc 退出":
        "When enabled, click the selected region's edge to add points continuously; press Esc to stop.",
    " · %d 个顶点 · ": " · %d vertices · ",

    # Ground localization settings
    "在「区域设置」点「画地面标定」, 依次点击地面上已知矩形的四个角, 填写实际宽度和纵深并保存。没有标定时地面定位不会产生效果。":
        "In Regions, choose Draw ground calibration, click the four corners of a known ground rectangle, then enter its actual width and depth and save. Ground localization has no effect without calibration.",

    # Region roles and recommendations
    "自家区域": "Private area",
    "花园、院子、车位: 有人进入就告警":
        "Garden, yard or parking space: alert when someone enters",
    "公共区域": "Public area",
    "马路、人行道: 只记录和计数, 不建议告警":
        "Road or sidewalk: record and count without alerts",
    "忽略区": "Ignored area",
    "摇晃的树、邻居窗户: 里面的目标一律不处理":
        "Moving trees, neighbouring windows: ignore all targets inside",
    "地面标定": "Ground calibration",
    "地面上一个矩形的 4 个角, 定位系统用来换算米":
        "Four corners of a ground rectangle, used by location mapping for metric coordinates",
    "地面上一个矩形的 4 个角, 地面定位用来换算米":
        "Four corners of a ground rectangle, used by ground mapping for metric coordinates",
    "未标注": "Unlabeled",
    "警戒区": "Protected zone",
    "警戒区 %d": "Protected zone %d",
    "电子围栏 %d": "Electronic fence %d",
    "地面标定 %d": "Ground calibration %d",
    "花园": "Garden",
    "花园 %d": "Garden %d",
    "院子": "Yard",
    "院子 %d": "Yard %d",
    "车位": "Parking space",
    "车位 %d": "Parking space %d",
    "门口": "Entrance",
    "门口 %d": "Entrance %d",
    "防区": "Zone",
    "防区 %d": "Zone %d",
    "区域 %d": "Region %d",
    "进出线 %d": "Crossing line %d",
    "马路": "Road",
    "马路 %d": "Road %d",
    "人行道": "Sidewalk",
    "人行道 %d": "Sidewalk %d",
    "通道": "Path",
    "通道 %d": "Path %d",
    "车辆经常经过": "Frequent vehicle traffic",
    "行人经过, 紧挨着马路": "Pedestrian traffic beside a road",
    "行人经常经过": "Frequent pedestrian traffic",
    "热图里没有明显集中的活动区域, 可以再多学习一段时间。":
        "The heatmap has no clear activity cluster yet; let it learn for longer.",
    "根据人 %d 次 / 车 %d 次的活动推荐了 %d 个区域, 请检查后再保存。":
        "From %d person and %d vehicle observations, suggested %d regions; review them before saving.",
    "根据人 %d 次 / 车 %d 次的活动推荐了 %d 个区域、%d 条进出线, 请检查后再保存。":
        "From %d person and %d vehicle observations, suggested %d regions and %d crossing lines; "
        "review them before saving.",
    " 进出线": " crossing line",
    "这里经常有人凭空出现/消失 (%d 次), 像是门口":
        "People often appear or disappear here (%d times); looks like an entrance",
    "箭头指向门口 = 进门": "Arrow points at the entrance = entering",
    "人流方向一致, 放在通道最窄处; 箭头 = 主要人流方向":
        "Consistent walking direction; placed at the narrowest point, arrow = main direction",
    "夜里 (%02d:00-%02d:00) 几乎没人, 可以只在夜间布防入侵":
        "Almost nobody here at night (%02d:00-%02d:00); consider arming intrusion only at night",
    "向%s %.1f%%": "%s %.1f%%",
    "%s旋转 %.1f°": "%s %.1f°",
    "右": "right",
    "左": "left",
    "下": "down",
    "上": "up",
    "顺时针": "clockwise",
    "逆时针": "counter-clockwise",
    "放大": "magnify",
    "缩小": "shrink",
    "、": ", ",
    "很小": "minimal",
    "统计到的目标太少 (人 %d / 车 %d), 先打开「学习」让它看一段时间, 建议覆盖白天和晚上。":
        "Too few observations (%d people / %d vehicles). Enable Learning for a while, ideally through day and night.",
    "区域「%s」至少要 3 个点": "Region \"%s\" needs at least 3 points",
    "区域「%s」的边自相交, 请调整顶点顺序":
        "Region \"%s\" has intersecting edges; adjust the vertex order",
    "区域「%s」面积太小": "Region \"%s\" is too small",
    "进出线「%s」太短": "Crossing line \"%s\" is too short",
    "地面标定「%s」必须正好 4 个点": "Ground calibration \"%s\" must have exactly 4 points",
    "地面标定「%s」要填实际宽和深 (米)":
        "Ground calibration \"%s\" needs positive width and depth in metres",
    "%s引用了不存在的线: %s": "%s references missing lines: %s",
    "%s引用了不存在的区域: %s": "%s references a missing region: %s",
    "%s不能使用「%s」(%s)": "%s cannot use \"%s\" (%s)",
    "%s已开启, 请在至少一个%s上启用它":
        "%s is enabled; enable it on at least one %s",
    "%s只能使用一个区域": "%s can use only one region",

    # Bird's-eye view, ground zones, multi-point calibration and device location
    "没有可用的地面标定": "No usable ground calibration",
    "设备位置: 未知": "Device location: unknown",
    "设备位置: 未开启": "Device location: off",
    "设备位置: 定位中 / 失败": "Device location: locating / failed",
    "设备位置: ": "Device location: ",
    "暂无统计": "No statistics yet",
    "当前 %d · 累计 %d 次 · 平均 %s 秒 · 最长 %s 秒": "Now %d · %d visits · avg %s s · max %s s",
    "标定 %s": "Calibration %s",
    "矩形 4 点": "4-point rectangle",
    "运行 %s": "Runtime %s",
    "质量": "Quality",
    "拟合残差": "Fit residual",
    "摄像头画面有偏移, 标定已自动跟随": "The camera view shifted; calibration follows automatically",
    "越界 / 无法换算": "Out of calibration / unmapped",
    "定位不确定度 / 抖动": "Position uncertainty / jitter",
    "空心 = 标定范围外": "Hollow = outside calibration",
    "残差 %s m": "residual %s m",
    "检查 %s m": "check %s m",
    "没有可用的地面标定; 在「区域设置」里画一个地面标定区域并启用定位": "No usable ground calibration; draw a ground calibration region in Regions and enable location mapping on it",
    "RMSE %s m · 最大 %s m · %d 个标定点": "RMSE %s m · max %s m · %d calibration points",
    "检查点误差": "Check point error",
    "没有检查点; 加几个检查点可以估计实际定位误差": "No check points; add a few to estimate the real positioning error",
    "画面覆盖": "Image coverage",
    "提示": "Notes",
    "%s · 最近 5 分钟 %d 次观测": "%s · %d observations in the last 5 minutes",
    "原因": "Reasons",
    "网格 %g 米 · y 轴指向远处": "Grid %g m · y axis points away",
    "正在读取鸟瞰数据…": "Loading bird's-eye data…",
    "定位系统未启用或没有可用的地面标定": "Location mapping is off or has no usable ground calibration",
    "%d 点": "%d points",
    "越界 %.0f%%": "out %.0f%%",
    "不确定度 %s m": "uncertainty %s m",
    "抖动 %s m": "jitter %s m",
    "只有 4 个标定点且没有检查点, 无法评估精度; 建议再量 1~2 个检查点":
        "Only 4 calibration points and no check points; accuracy cannot be estimated. Add 1–2 check points.",
    "地面标定无效, 请重新标定":
        "Ground calibration is invalid; recalibrate the ground area",
    "标定质量差, 建议增加标定点或检查点":
        "Calibration quality is poor; add calibration or check points",
    "摄像头画面有偏移, 标定在自动跟随; 偏移大时建议重新标定":
        "The camera view has shifted and calibration is following automatically; recalibrate if the shift is large",
    "多点对应": "Multi-point",
    "RMSE %s m · 最大 %s m · %d 个检查点": "RMSE %s m · max %s m · %d check points",
    "鸟瞰数据读取失败: %s": "Failed to read bird's-eye data: %s",
    "鸟瞰图 / 定位": "Bird's-eye view / Location",
    "地面区域的累计次数和停留时间清零": "Reset visit counts and dwell times of ground zones",
    "保存地面区域的修改": "Save ground zone changes",
    "按地面米坐标依次点击顶点, 双击或点回第一个点完成": "Click vertices in ground meters; double-click or click the first point to finish",
    "按坐标新建一个 2×2 米的区域, 再修改顶点坐标": "Create a 2×2 m zone, then edit its vertex coordinates",
    "停留超时告警 (秒)": "Dwell alarm after (seconds)",
    "0 = 不报停留": "0 = no dwell alarm",
    "检测服务未运行": "Detection service is not running",
    "清零": "Reset",
    "画面坐标": "Image coordinates",
    "米制坐标": "Metric coordinates",
    "已新建区域, 在右侧修改顶点坐标后点「保存区域」": "Zone created; edit its vertices on the right, then click Save zones",
    "清零统计": "Reset statistics",
    "保存区域": "Save zones",
    "在鸟瞰图上画区域": "Draw zone on map",
    "撤销一点": "Undo point",
    "地面区域": "Ground zone",
    "在鸟瞰图上按米画, 或在「区域设置」里在画面上画。可以设置越界进入告警和停留超时告警。": "Draw in meters on the bird's-eye view, or on the camera image in Regions. Entry and dwell alarms can be configured.",
    "在鸟瞰图上按米画, 或在「区域设置」里在画面上画。每个区域可单独启用、设置越界进入告警和停留超时告警；这些都受上方功能总开关约束。":
        "Draw in meters on the bird's-eye view, or on the camera image in Regions. Each zone can be enabled and have entry or dwell alarms; all are subject to the feature master switch above.",
    "选中区域": "Selected zone",
    "启用": "Enabled",
    "启用此区域": "Enable this zone",
    "只控制这个地面区域；功能总开关仍在「设置 > 地面定位与鸟瞰」中":
        "Controls this ground zone only; the feature master switch remains in Settings > Ground mapping and bird's-eye view",
    "停用后不统计、不告警": "When disabled, no statistics and no alarms",
    "进入时告警": "Alarm on entry",
    "关闭时进入只记一条信息事件": "When off, entering only records an info event",
    "目标分组": "Target groups",
    "都不勾 = 全部目标": "None checked = all targets",
    "顶点 (米): x, y; x, y; …": "Vertices (m): x, y; x, y; …",
    "画面上绘制的区域": "Zone drawn on the camera image",
    "在「区域设置」里调整形状; 按地面标定换算成米": "Edit its shape in Regions; converted to meters by the ground calibration",
    "删除这个区域": "Delete this zone",
    "删除后仍需点「保存区域」才会写入检测服务": "Click Save zones afterwards to apply the deletion",
    "标定与运行状态": "Calibration and runtime status",
    "标定在「区域设置」的地面标定区域里修改; 加检查点可以估计实际误差。": "Edit the calibration on the ground calibration region in Regions; check points estimate the real error.",
    "设备位置": "Device location",
    "设备经纬度": "Device coordinates",
    "经纬度": "Latitude / longitude",
    "立即定位": "Locate now",
    "正在重新定位, 稍后更新": "Locating; the result will update shortly",
    "清零地面区域统计?": "Reset ground zone statistics?",
    "所有地面区域的累计次数和停留时间会清零, 不影响事件记录。": "Visit counts and dwell times of all ground zones will be reset. Events are not affected.",
    "已停用": "Disabled",
    "画地面区域: 在鸟瞰图上依次点击顶点 (已 %d 个点, 按 5 厘米取整)。双击、点回第一个点或按「完成」结束; 右键撤销上一个点。": "Draw ground zone: click vertices on the map (%d so far, snapped to 5 cm). Double-click, click the first point or press Finish; right-click undoes the last point.",
    "已选区域: 在右侧修改名称、告警和顶点坐标; 点「保存区域」写入检测服务。": "Zone selected: edit name, alarms and vertices on the right; click Save zones to apply.",
    "点选鸟瞰图上的区域查看统计和修改; 实心点 = 标定范围内的目标, 空心 = 范围外。": "Click a zone on the map to view statistics and edit it; filled dots = targets inside calibration, hollow = outside.",
    "先完成或取消正在画的区域": "Finish or cancel the zone being drawn first",
    "地面区域已保存": "Ground zones saved",
    "地面区域的改动还没有保存。": "Ground zone changes have not been saved.",
    "还没有地面区域, 点「在鸟瞰图上画区域」开始": "No ground zones yet; click Draw zone on map to start",
    "鸟瞰数据": "Bird's-eye data",
    "统计已清零": "Statistics reset",
    "进入告警": "entry alarm",
    "无法换算": "cannot be mapped",
    "未保存 / 暂无统计": "Unsaved / no statistics yet",
    "重新定位失败: %s": "Locating failed: %s",
    "停留 %g 秒告警": "dwell alarm after %g s",
    "未启用或没有可用的地面标定 (在「设置」里打开定位系统)": "Off or no usable ground calibration (enable Location mapping in Settings)",
    "清零失败: %s": "Reset failed: %s",
    "地面标定「%s」至少要 %d 个标定点 (现有 %d 个)": "Ground calibration \"%s\" needs at least %d calibration points (has %d)",
    "地面标定「%s」的标定点在画面上几乎共线, 请分散在地面各处": "Calibration points of \"%s\" are almost collinear in the image; spread them over the ground",
    "地面标定「%s」的标定点地面坐标几乎共线, 请检查 X / Y": "Ground coordinates of \"%s\" are almost collinear; check X / Y",
    "地面区域「%s」必须且只能有米制坐标或画面坐标之一": "Ground zone \"%s\" must have exactly one of metric or image coordinates",
    "地面区域「%s」的坐标格式不对": "Ground zone \"%s\" has invalid coordinates",
    "地面区域「%s」至少要 3 个点": "Ground zone \"%s\" needs at least 3 points",
    "地面区域「%s」的边自相交, 请调整顶点顺序": "Edges of ground zone \"%s\" intersect; reorder its vertices",
    "地面区域「%s」的画面坐标超出画面": "Image coordinates of ground zone \"%s\" are outside the image",
    "坐标格式应为「x, y; x, y; …」": "Coordinates must look like \"x, y; x, y; …\"",
    "地面区域 %d": "Ground zone %d",
    "地面标定「%s」第 %d 个检查点格式不对 (需要 u, v, X, Y)": "Ground calibration \"%s\": check point %d is invalid (needs u, v, X, Y)",
    "地面标定「%s」第 %d 个标定点格式不对 (需要 u, v, X, Y)": "Ground calibration \"%s\": calibration point %d is invalid (needs u, v, X, Y)",
    "地面区域「%s」面积太小": "Ground zone \"%s\" is too small",
    "地面区域「%s」的停留时间不对": "Ground zone \"%s\" has an invalid dwell time",
    "地面区域「%s」的分组「%s」不存在": "Ground zone \"%s\": group \"%s\" does not exist",
    "地面标定「%s」第 %d、%d 个标定点在画面上重合": "Ground calibration \"%s\": points %d and %d overlap in the image",
    "地面标定「%s」第 %d、%d 个标定点的地面坐标相同": "Ground calibration \"%s\": points %d and %d have the same ground coordinates",
    "定位系统用的地面区域 (越界进入 / 停留统计), 按地面标定换算成米; 告警在「鸟瞰图 / 定位」里设置": "Ground zone for location mapping (entry / dwell statistics), converted to meters by the ground calibration; alarms are set in Bird's-eye view",
    "地面定位用的地面区域 (越界进入 / 停留统计), 按地面标定换算成米; 告警在「鸟瞰图 / 定位」里设置":
        "Ground zone for ground mapping (entry / dwell statistics), converted to meters by the ground calibration; alarms are set in Bird's-eye view",
    "删除这个点": "Delete this point",
    "未评估": "Not evaluated",
    "画地面区域": "Draw ground zone",
    "标定方式": "Calibration mode",
    "标定点与精度": "Calibration points and accuracy",
    "在画面上点地面上已知位置的点 (地砖角、车位线角、桩子脚), 再填它的地面坐标 X / Y (米, y 指向远处)。点越分散、覆盖越广越准。": "Click points on the ground with known positions (tile corners, parking line corners, post bases) and enter their ground X / Y (meters, y points away). Spread-out points give better accuracy.",
    "标定精度": "Calibration accuracy",
    "评估": "Evaluate",
    "标定点": "Calibration points",
    "至少 4 个, 不要在一条直线上; 参与拟合": "At least 4, not on one line; used for fitting",
    "在画面上添加": "Add on image",
    "检查点": "Check points",
    "可选, 不参与拟合, 用来估计实际定位误差": "Optional; not used for fitting, estimates the real positioning error",
    "地面区域 · %s": "Ground zone · %s",
    "按地面标定换算成米, 画在地面上的边界上": "Converted to meters by the ground calibration; draw along boundaries on the ground",
    "地面坐标 (米)": "Ground coordinate (m)",
    "已切换到多点标定: 原来的 4 个角已转成标定点, 可以继续添加并修改地面坐标": "Switched to multi-point calibration: the 4 corners became calibration points; add more and edit their ground coordinates",
    "评估中…": "Evaluating…",
    "检测服务未运行, 无法评估; 保存后由检测服务计算": "Detection service is not running; it will evaluate after saving",
    "画地面区域: 沿地面上的边界依次点击 (已 %d 个点)。点回第一个点、双击或按回车完成; 右键/退格撤销上一个点; Esc 取消。需要有地面标定才能换算成米。": "Draw ground zone: click along the boundary on the ground (%d points). Click the first point, double-click or press Enter to finish; right-click/Backspace undoes; Esc cancels. A ground calibration is required to convert to meters.",
    " · 告警设置在「鸟瞰图 / 定位」里": " · alarms are set in Bird's-eye view",
    "画面 (%.3f, %.3f)": "Image (%.3f, %.3f)",
    "添加%s: 在画面上点击地面上已知位置的点, 然后在右侧填它的地面坐标 X / Y (米)。可连续添加; 按「完成」、右键或 Esc 结束。": "Add %s: click a ground point with a known position, then enter its ground X / Y (m) on the right. Add several in a row; press Finish, right-click or Esc to stop.",
    "地面区域 · %d 个顶点 · 定位越界 / 停留统计": "Ground zone · %d vertices · entry / dwell statistics",
    "已开启 · 在「设置 > 地面定位与鸟瞰」中修改":
        "On · change it in Settings > Ground mapping and bird's-eye view",
    "未开启 · 二维定位、地面区域统计和鸟瞰图画中画均不运行；在「设置 > 地面定位与鸟瞰」中打开":
        "Off · ground mapping, zone statistics and bird's-eye picture-in-picture do not run; turn it on in Settings > Ground mapping and bird's-eye view",
    "多点标定: 区域轮廓 = 标定点在画面上的凸包": "Multi-point calibration: outline = convex hull of the calibration points",
    "定位系统: 越界 / 停留统计": "Location mapping: entry / dwell statistics",
    "检查点 C%d": "Check point C%d",
    "标定点 %d": "Calibration point %d",
    "标定无效: %s": "Calibration invalid: %s",
    "多点地面标定: 拖动黄点调整位置, 右键黄点删除; 在右侧添加标定点 / 检查点并填地面坐标。": "Multi-point ground calibration: drag yellow points to adjust, right-click to delete; add calibration / check points on the right and enter ground coordinates.",
    "地面区域: 拖动白点改形状, 拖动内部平移, 双击边缘加点, 右键白点删点; 方向键可微调位置; 告警和停留时间在「鸟瞰图 / 定位」里设置。": "Ground zone: drag white points to reshape, drag inside to move, double-click an edge to add a point, right-click a point to delete. Arrow keys nudge it; alarms and dwell time are set in Bird's-eye view.",
    "精度 (米)": "Accuracy (m)",
    "鸟瞰图 / 定位 (Ctrl+G)": "Bird's-eye view / Location (Ctrl+G)",
    "放大鸟瞰图": "Zoom in bird's-eye view",
    "缩小鸟瞰图": "Zoom out bird's-eye view",
    "恢复到适合窗口": "Reset to fit to window",
    "滚轮也可以缩放鸟瞰图": "Use the mouse wheel to zoom the bird's-eye view",
    "滚轮缩放; 点适合窗口按钮恢复 100%":
        "Scroll to zoom; use Fit to window to return to 100%",
    "画中画位置已保存": "Picture-in-picture position saved",
    "定位方式已保存": "Location methods saved",
    "定位顺序已保存": "Location order saved",
    "Google API Key 已保存": "Google API key saved",
    "手动位置已保存": "Manual location saved",
    "鸟瞰图画中画": "Bird's-eye picture-in-picture",
    "在监控画面角落叠加定位鸟瞰图 (APP、Web、录像和快照都能看到), 需开启定位系统。": "Overlay the bird's-eye map in a corner of the video (visible in the APP, Web, recordings and snapshots). Requires Location mapping.",
    "地面定位与鸟瞰": "Ground mapping and bird's-eye view",
    "地面标定": "Ground calibration",
    "打开区域设置": "Open Regions",
    "在画面上画地面上已知矩形的四个角, 并填写实际宽度和纵深":
        "Draw four corners of a known ground rectangle and enter its actual width and depth",
    "调整形状": "Adjust shape",
    "未完成 · 先在「区域设置」画地面标定, 填写实际宽深并保存":
        "Incomplete · draw ground calibration in Regions, enter the actual width and depth, then save",
    "待地面定位总开关开启后验证":
        "Will be verified after the ground mapping master switch is on",
    "当前地面区域的修改还没有保存, 请先点「保存区域」":
        "Save the current ground zone changes first",
    "地面定位只负责把目标换算成地面坐标、统计地面区域并绘制鸟瞰图。区域形状和标定在「区域设置」中，单区域行为在「鸟瞰图 / 定位」中。":
        "Ground mapping converts targets to ground coordinates, tracks ground zones and draws the bird's-eye view. Shapes and calibration are in Regions; per-zone behavior is in Bird's-eye view / Location.",
    "先完成地面标定并绑定，再打开总开关；后端会拒绝没有地面标定绑定的开启。区域形状和标定在「区域设置」中，单区域行为在「鸟瞰图 / 定位」中。":
        "Complete and bind ground calibration before turning on the master switch; the service rejects the switch without a calibration binding. Shapes and calibration are in Regions; per-zone behavior is in Bird's-eye view / Location.",
    "第一步：画地面标定并绑定":
        "Step 1: Draw and bind ground calibration",
    "开启地面定位": "Enable ground mapping",
    "地面定位总开关": "Ground mapping master switch",
    "开关打开后还需要地面标定":
        "Ground calibration is still required after enabling the switch",
    "在「区域设置」点「画地面标定」, 依次点地面上已知矩形的四个角, 再填写实际宽度和纵深并保存。没有标定时地面定位不会产生效果。":
        "In Regions, click Draw ground calibration, click the four corners of a known ground rectangle, then enter and save its actual width and depth. Ground mapping has no effect without calibration.",
    "功能总开关：关闭后不计算地面坐标、不统计地面区域，也不显示鸟瞰图画中画。":
        "Feature master switch. When off, ground coordinates and zone statistics are not calculated and bird's-eye picture-in-picture is hidden.",
    "第二步：标定保存后打开。关闭后不计算地面坐标、不统计地面区域，也不显示鸟瞰图画中画。":
        "Step 2: turn on after saving calibration. When off, ground coordinates and zone statistics are not calculated and bird's-eye picture-in-picture is hidden.",
    "还不能开启地面定位: 先在「区域设置 > 画地面标定」完成四个角和实际宽深并保存":
        "Ground mapping cannot be enabled yet: in Regions > Draw ground calibration, mark the four corners, enter the actual width and depth, and save",
    "在监控画面角落叠加地面定位鸟瞰图 (APP、Web、录像和快照都能看到)。这是显示开关，只有上方的地面定位总开关开启后才会显示。":
        "Overlay the ground mapping bird's-eye view in a corner of the video (visible in the APP, Web, recordings and snapshots). This is a display switch and only works while the ground mapping master switch above is on.",
    "显示鸟瞰图画中画": "Show bird's-eye picture-in-picture",
    "画出标定范围、地面区域、目标位置和轨迹": "Draws calibration area, ground zones, target positions and trails",
    "宽度 (占画面宽度 %)": "Width (% of video width)",
    "15–50%, 默认 20%": "15–50%, default 20%",
    "盒子所在的经纬度, 显示在鸟瞰图上并随事件上报。按下面的顺序尝试定位方式, 第一个成功的为准。": "Latitude and longitude of the box, shown on the bird's-eye view and reported with events. Methods are tried in order; the first success wins.",
    "这里是盒子自身的经纬度，与上方的地面定位总开关相互独立。位置会显示在鸟瞰图上并随事件上报；按下面的顺序尝试来源，第一个成功的为准。":
        "Coordinates of the box itself, independent of the ground mapping master switch above. The location appears on the bird's-eye view and in events; sources are tried in order and the first success wins.",
    "自动获取设备位置": "Determine device location",
    "自动获取设备经纬度": "Determine device coordinates",
    "定位总开关未开启，画中画不会显示":
        "Ground mapping master switch is off; picture-in-picture will not be shown",
    "当前位置": "Current location",
    "重新定位间隔 (分钟)": "Relocate interval (minutes)",
    "1–1440 分钟": "1–1440 minutes",
    "定位方式": "Location methods",
    "打开的方式按从上到下的顺序尝试; 箭头调整顺序。": "Enabled methods are tried from top to bottom; use the arrows to reorder.",
    "Google API Key (Wi-Fi 定位用)": "Google API key (for Wi-Fi location)",
    "手动位置": "Manual location",
    "「手动填写」方式使用这里的经纬度; 纬度和经度都留空表示不设置。": "The Manual method uses these coordinates; leave latitude and longitude empty to unset.",
    "纬度 (-90～90)": "Latitude (-90 to 90)",
    "经度 (-180～180)": "Longitude (-180 to 180)",
    "地址 (可选)": "Address (optional)",
    "保存手动位置": "Save manual location",
    "鸟瞰图画中画已%s": "Bird's-eye picture-in-picture %s",
    "画中画宽度已设为画面的 %d%%": "Picture-in-picture width set to %d%% of the video",
    "设备定位已%s": "Device location %s",
    "每 %d 分钟重新定位一次": "Relocating every %d minutes",
    "检测服务未运行或尚未定位": "Detection service not running or not located yet",
    "正在保存配置, 请稍后再打开鸟瞰图": "Saving configuration; open the bird's-eye view later",
    "至少保留一种定位方式; 不需要定位请关闭「自动获取设备位置」": "Keep at least one method; turn off Determine device location if not needed",
    "至少保留一种定位方式; 不需要设备经纬度请关闭「自动获取设备经纬度」":
        "Keep at least one source; turn off Determine device coordinates if not needed",
    "未启用或没有可用的地面标定 (在「设置 > 地面定位与鸟瞰」里打开)":
        "Off or no usable ground calibration (enable it in Settings > Ground mapping and bird's-eye view)",
    "检测服务未运行, 无法定位": "Detection service is not running; cannot locate",
    "纬度范围 -90～90, 经度范围 -180～180": "Latitude must be -90 to 90 and longitude -180 to 180",
    "纬度 / 经度要填数字, 例如 31.2304, 121.4737": "Latitude / longitude must be numbers, e.g. 31.2304, 121.4737",
    "良好": "good",
    "一般": "fair",
    "较差": "poor",
    "无效": "invalid",
    "精度下降": "degraded",
    "失效": "failed",
    "无目标": "no targets",
    "Wi-Fi 定位": "Wi-Fi",
    "IP 定位": "IP location",
    "手动": "manual",
    "人员": "People",
    "车辆": "Vehicles",
    "宠物": "Pets",
    "其它": "Other",
    "多点对应 (≥4 点)": "Multi-point (≥4 points)",
    "左上": "Top left",
    "右上": "Top right",
    "左下": "Bottom left",
    "右下": "Bottom right",
    "进入地面区域": "Entered ground zone",
    "离开地面区域": "Left ground zone",
    "区域停留": "Zone dwell",
    "Wi-Fi 定位 (Google)": "Wi-Fi location (Google)",
    "扫描附近 Wi-Fi 热点, 需要 Google API Key, 精度几十米": "Scans nearby Wi-Fi access points; needs a Google API key; tens of meters accuracy",
    "按公网 IP 粗略定位, 城市级精度": "Rough location from the public IP; city-level accuracy",
    "手动填写": "Manual",
    "使用下面填写的经纬度": "Uses the coordinates entered below",
    "人员/车辆/宠物地面二维定位、轨迹、区域越界与停留": "2D ground positioning of people / vehicles / pets, trails, zone entry and dwell",
    "上移": "Move up",
    "下移": "Move down",
    "左移": "Move left",
    "右移": "Move right",
    "位置微调": "Position nudge",
    "拖动区域内部整体平移，方向键微调；Shift 加速": "Drag the region interior to move it, or use the arrow keys to nudge; hold Shift for larger steps",
    "点选区域或线后可拖动顶点；拖区域内部可整体上下左右移动，方向键可微调。": "After selecting a region or line, drag a vertex to reshape it. Drag the interior to move the whole item, or use the arrow keys to nudge it.",
    "未知": "unknown",
}


_NUMBERED_NAMES = (
    (re.compile(r"^警戒区\s*([0-9]+)$"), "Protected zone %s"),
    (re.compile(r"^电子围栏\s*([0-9]+)$"), "Electronic fence %s"),
    (re.compile(r"^地面标定\s*([0-9]+)\s*地面区域$"), "Ground calibration %s ground zone"),
    (re.compile(r"^地面标定\s*([0-9]+)$"), "Ground calibration %s"),
    (re.compile(r"^自家区域\s*([0-9]+)$"), "Private area %s"),
    (re.compile(r"^公共区域\s*([0-9]+)$"), "Public area %s"),
    (re.compile(r"^忽略区\s*([0-9]+)$"), "Ignored area %s"),
    (re.compile(r"^花园\s*([0-9]+)$"), "Garden %s"),
    (re.compile(r"^院子\s*([0-9]+)$"), "Yard %s"),
    (re.compile(r"^车位\s*([0-9]+)$"), "Parking space %s"),
    (re.compile(r"^防区\s*([0-9]+)$"), "Zone %s"),
    (re.compile(r"^区域\s*([0-9]+)$"), "Region %s"),
    (re.compile(r"^进出线\s*([0-9]+)$"), "Crossing line %s"),
    (re.compile(r"^马路\s*([0-9]+)$"), "Road %s"),
    (re.compile(r"^人行道\s*([0-9]+)$"), "Sidewalk %s"),
    (re.compile(r"^通道\s*([0-9]+)$"), "Path %s"),
    (re.compile(r"^门口\s*([0-9]+)$"), "Entrance %s"),
    (re.compile(r"^地面区域\s*([0-9]+)$"), "Ground zone %s"),
)


_DYNAMIC_PATTERNS = (
    (re.compile(r"^第 ([0-9]+) 个标定点残差 ([0-9.]+)m, 可能点错或量错$"),
     "Calibration point %s has a %s m residual; it may be misplaced or measured incorrectly"),
    (re.compile(r"^第 ([0-9]+) 个检查点误差 ([0-9.]+)m$"),
     "Check point %s error: %s m"),
    (re.compile(r"^标定点只覆盖画面 ([0-9.]+)%, 标定范围外的位置误差会很大$"),
     "Calibration points cover only %s%% of the image; positions outside the calibrated area will have much larger errors"),
    (re.compile(r"^([0-9.]+)% 的脚点在地平线以上无法换算, 标定可能已失效$"),
     "%s%% of foot points are above the horizon and cannot be mapped; calibration may be invalid"),
    (re.compile(r"^([0-9.]+)% 的位置在标定范围外, 建议扩大标定范围$"),
     "%s%% of positions are outside the calibrated area; consider expanding it"),
    (re.compile(r"^平均定位不确定度 ([0-9.]+)m, 目标多在远处$"),
     "Mean position uncertainty is %s m; targets are mostly far away"),
    (re.compile(r"^静止目标位置抖动 ([0-9.]+)m$"),
     "Stationary target position jitter is %s m"),
)


def t(text):
    """Translate a user-facing source string, returning the source on fallback."""
    if _language == "en":
        translated = _EN.get(text)
        if translated is not None:
            return translated
        for pattern, template in _NUMBERED_NAMES:
            match = pattern.match(text)
            if match:
                return template % match.group(1)
        for pattern, template in _DYNAMIC_PATTERNS:
            match = pattern.match(text)
            if match:
                return template % match.groups()
    return text


_RUNTIME_PATTERNS = (
    (re.compile(r"^目标丢失, 在 (.+) 共停留 ([0-9.]+)s$"),
     "Target lost after %s in %s"),
    (re.compile(r"^离开 (.+), 共停留 ([0-9.]+)s$"),
     "Left %s after %s seconds"),
    (re.compile(r"^离开 (.+)$"), "Left %s"),
    (re.compile(r"^进入 (.+), 已持续 ([0-9.]+)s$"),
     "Entered %s; sustained %s seconds"),
    (re.compile(r"^穿过 (.+), 进 ([0-9]+) 出 ([0-9]+)$"),
     "Crossed %s; IN %s OUT %s"),
    (re.compile(r"^滞留: (.+) 已停留 ([0-9.]+)s$"),
     "Loitering: %s for %s seconds"),
    (re.compile(r"^超长滞留: (.+) 已停留 ([0-9.]+)s$"),
     "Extended loitering: %s for %s seconds"),
    (re.compile(r"^(.+) 内 ([0-9]+) 人已停留 ([0-9.]+)s$"),
     "%s: %s people loitered for %s seconds"),
    (re.compile(r"^物体静止 ([0-9.]+)s$"), "Object stationary for %s seconds"),
    (re.compile(r"^周边无人 ([0-9.]+)s$"), "No nearby person for %s seconds"),
    (re.compile(r"^物体已消失 ([0-9.]+)s \(此前静止 ([0-9.]+)s\)$"),
     "Object disappeared after %s seconds (previously stationary for %s seconds)"),
    (re.compile(r"^位置 \(([-0-9.]+), ([-0-9.]+)\)m$"), "Position (%s, %s) m"),
)


def runtime_text(text):
    """Translate server-generated event details without changing stored events."""
    text = str(text or "")
    if _language != "en" or not text:
        return text
    exact = _EN.get(text)
    if exact is not None:
        return exact
    if text == "位置超出标定地面, 结果仅供参考":
        return "Position is outside the calibrated ground area; result is approximate"
    for pattern, template in _RUNTIME_PATTERNS:
        match = pattern.match(text)
        if match:
            values = [t(value) if i == 0 else value for i, value in enumerate(match.groups())]
            return template % tuple(values)
    direction = text.replace("向右", "right").replace("向左", "left")
    direction = direction.replace("、远离镜头", ", away from camera")
    direction = direction.replace("、靠近镜头", ", toward camera")
    return direction

