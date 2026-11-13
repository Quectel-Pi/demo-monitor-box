'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const clone = (v) => JSON.parse(JSON.stringify(v));
const esc = (v) => String(v ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const fmt = (v, digits = 0) => (typeof v === 'number' && Number.isFinite(v)) ? v.toFixed(digits) : '—';

const APP_I18N = window.MB_APP_I18N || {};
const translatedTextSources = new WeakMap();
const translatedAttrSources = new WeakMap();
const LANGUAGE_VALUES = new Set(['zh_CN', 'en']);
let currentLanguage = 'zh_CN';
let localizeScheduled = false;

function normalizeLanguage(value) {
  return String(value || '').toLowerCase().startsWith('en') ? 'en' : 'zh_CN';
}

const nativeConfirm = window.confirm.bind(window);
const nativeAlert = window.alert.bind(window);
window.confirm = (message) => nativeConfirm(translatePhrase(String(message)));
window.alert = (message) => nativeAlert(translatePhrase(String(message)));

function translateExact(text) {
  if (currentLanguage !== 'en') return text;
  return APP_I18N[text] ?? text;
}

function translatePhrase(text) {
  if (currentLanguage !== 'en') return text;
  let out = translateExact(text);
  if (out !== text) return out;
  const rules = [
    [/^共 (\d+) 条 · 当前显示 (\d+) 条$/, 'Total $1 · showing $2'],
    [/^已加载 (\d+) 批$/, 'Loaded $1 batches'],
    [/^第 (\d+) 批$/, 'Batch $1'],
    [/^删除所选 \((\d+)\)$/, 'Delete selected ($1)'],
    [/^已删除 (\d+) 条$/, 'Deleted $1'],
    [/^已加载 (\d+) 个建议区域，请检查后绑定功能并保存$/, '$1 suggested regions created. Review, bind features and save.'],
    [/^已根据热图生成 (\d+) 个建议区域，请检查后绑定功能并保存；区域已达 (\d+) 个上限，未添加 (\d+) 个建议$/,
      '$1 suggested regions created. Review, bind features and save. The $2-region limit was reached; $3 suggestions were not added.'],
    [/^(\d+) 项$/, '$1 items'],
    [/^(\d+) 个目标 · (\d+) 个地面区域$/, '$1 targets · $2 ground zones'],
    [/^定位运行中 · (.+)$/, 'Localization running · $1'],
    [/^(\d+) 秒前$/, '$1 seconds ago'],
    [/^标定 (.+) · (.+) m$/, 'Calibration $1 · $2 m'],
    [/^已保存 (\d+) 条$/, 'Saved $1 events'],
    [/^已删除 (\d+) 条，(.+)$/, 'Deleted $1; $2'],
    [/^(.+)，(\d+) 条正在录像$/, '$1; $2 clips still recording'],
    [/^(.+)，(\d+) 条失败$/, '$1; $2 failed'],
    [/^区域已保存；需重启：(.+)$/, 'Regions saved; restart required: $1'],
    [/^配置已保存；需重启 mb-capture 生效：(.+)$/, 'Configuration saved; restart mb-capture to apply: $1'],
    [/^(.+)已开启，请先绑定至少一条进出线$/, '$1 is enabled; bind at least one crossing line'],
    [/^(.+)已开启，请先绑定至少一个区域$/, '$1 is enabled; bind at least one region'],
    [/^(.+)已开启$/, '$1 enabled'],
    [/^(.+)已关闭$/, '$1 disabled'],
    [/^(.+)不能绑定到“(.+)”$/, '$1 cannot be bound to “$2”'],
    [/^“(.+)”不能绑定功能 (.+)$/, '“$1” cannot bind feature $2'],
    [/^区域“(.+)”至少需要 3 个点$/, 'Region “$1” requires at least 3 points'],
    [/^区域“(.+)”的边自相交$/, 'Region “$1” has intersecting edges'],
    [/^地面区域“(.+)”至少需要 3 个点$/, 'Ground zone “$1” requires at least 3 points'],
    [/^地面区域“(.+)”的边自相交$/, 'Ground zone “$1” has intersecting edges'],
    [/^地面标定“(.+)”必须正好 4 个点$/, 'Ground calibration “$1” requires exactly 4 points'],
    [/^地面标定“(.+)”至少需要 4 个标定点$/, 'Ground calibration “$1” requires at least 4 calibration points'],
    [/^地面标定“(.+)”的标定点格式不对$/, 'Ground calibration “$1” has malformed calibration points'],
    [/^地面标定“(.+)”需要填写实际宽度和纵深$/, 'Ground calibration “$1” requires real width and depth'],
    [/^进出线“(.+)”太短$/, 'Crossing line “$1” is too short'],
    [/^区域最多只能有 (\d+) 个$/, 'At most $1 regions are allowed'],
    [/^区域最多只能有 (\d+) 个；请先删除不用的区域$/, 'At most $1 regions are allowed; delete an unused region first'],
    [/^删除“(.+)”？.*$/, 'Delete “$1”? This is applied after you select Save regions.'],
    [/^(.+)（还有 (\d+) 处）$/, '$1 ($2 more)'],
    [/^(\d+) 个地面区域$/, '$1 ground zones'],
    [/^当前 (\d+) · 累计 (\d+) 次 · 平均 (.+) 秒$/, 'Now $1 · Total $2 · Avg $3 s'],
    [/^(\d+) targets$/, '$1 targets'],
    [/^(\d+) 条$/, '$1 events'],
    [/^净 (-?\d+)$/, 'Net $1'],
    [/^(\d+(?:\.\d+)?) 秒$/, '$1 s'],
    [/^(.+) · 已连接$/, '$1 · Connected'],
    [/^(.+) · 断开$/, '$1 · Disconnected'],
    [/^(\d+) 个目标$/, '$1 targets'],
    [/^(\d+) 个地面区域$/, '$1 ground zones']
  ];
  for (const [re, replacement] of rules) {
    if (re.test(out)) return out.replace(re, replacement);
  }
  return out;
}

function translateNodeValue(value) {
  const match = String(value).match(/^(\s*)([\s\S]*?)(\s*)$/);
  if (!match || !match[2]) return value;
  const translated = translatePhrase(match[2]);
  return translated === match[2] ? value : match[1] + translated + match[3];
}

function shouldLocalizeNode(node) {
  const el = node.parentElement;
  if (!el) return false;
  return !el.closest('script,style,textarea,pre,code,[data-i18n-skip]');
}

function localizeDom(root = document.body) {
  if (!root) return;
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    if (!shouldLocalizeNode(node)) continue;
    if (!translatedTextSources.has(node)) translatedTextSources.set(node, node.nodeValue);
    const source = translatedTextSources.get(node);
    const next = currentLanguage === 'en' ? translateNodeValue(source) : source;
    if (node.nodeValue !== next) node.nodeValue = next;
  }
  if (root.querySelectorAll) {
    for (const el of [root, ...root.querySelectorAll('*')]) {
      if (!el || el.closest?.('script,style,[data-i18n-skip]')) continue;
      for (const attr of ['title', 'aria-label', 'placeholder', 'alt']) {
        if (!el.hasAttribute(attr) || el.matches('input[type=text],input[type=number],input[type=password]')) continue;
        let sourceMap = translatedAttrSources.get(el);
        if (!sourceMap) { sourceMap = new Map(); translatedAttrSources.set(el, sourceMap); }
        if (!sourceMap.has(attr)) sourceMap.set(attr, el.getAttribute(attr));
        const source = sourceMap.get(attr);
        const next = currentLanguage === 'en' ? translatePhrase(source) : source;
        if (el.getAttribute(attr) !== next) el.setAttribute(attr, next);
      }
    }
  }
}

function scheduleLocalize() {
  if (localizeScheduled) return;
  localizeScheduled = true;
  requestAnimationFrame(() => {
    localizeScheduled = false;
    localizeDom(document.body);
  });
}

function rerenderLocalizedUi() {
  if (S.config && Object.keys(S.config).length) applyConfigForm();
  if (S.status && Object.keys(S.status).length) renderStatus();
  if (S.events) renderEvents();
  renderRegionList();
  renderRegionInspector();
  renderGround();
  renderGeoSummary();
  scheduleLocalize();
}

function setLanguage(value, options = {}) {
  currentLanguage = normalizeLanguage(value);
  S.language = currentLanguage;
  document.documentElement.lang = currentLanguage === 'en' ? 'en' : 'zh-CN';
  const top = $('#web-language');
  if (top) top.value = currentLanguage;
  const settings = $('#settings-language');
  if (settings) settings.value = currentLanguage;
  document.title = translateExact('监控盒子');
  if (options.rerender !== false) rerenderLocalizedUi();
  scheduleLocalize();
}

async function changeLanguage(value) {
  const language = normalizeLanguage(value);
  if (!LANGUAGE_VALUES.has(language)) return;
  const previous = currentLanguage;
  setLanguage(language);
  S.languageHoldUntil = Date.now() + 4000;
  try {
    await saveConfigPatch({app: {language}}, {message: '界面语言已保存', keepRegions: true});
  } catch (err) {
    setLanguage(previous);
    toast(err.message, 'error');
  }
}
const pct = (v, digits = 0) => (typeof v === 'number' && Number.isFinite(v)) ? `${(v * 100).toFixed(digits)}%` : '—';
const timeText = (s) => String(s || '').replace('T', ' ').replace(/(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)?$/, '');
const shortTime = (s) => {
  const t = timeText(s);
  return t.length > 16 ? t.slice(11, 16) : t;
};

const VIEW_META = {
  live: ['实时监控', '摄像头画面、告警和运行概况'],
  events: ['告警事件', '浏览快照、回放录像和清理历史事件'],
  regions: ['区域设置', '在画面上绘制区域、进出线和地面标定'],
  ground: ['鸟瞰图 / 定位', '地面坐标、轨迹、区域统计和设备位置'],
  settings: ['设置', '检测能力、视频、显示、定位和事件策略'],
  status: ['运行状态', '链路指标、事件统计和配置信息']
};
const FEATURES = [
  ['object', '物体检测', '检测并显示 COCO 80 类物体'],
  ['intrusion', '入侵检测', '有人进入警戒区时告警'],
  ['fall', '跌倒检测', '识别人员跌倒姿态'],
  ['count', '人数统计', '按进出线累计人数'],
  ['fire', '火焰烟雾', '专用模型和时序确认'],
  ['fence', '电子围栏', '目标跨越围栏线时告警'],
  ['dwell', '人员滞留', '目标在区域停留过久时告警'],
  ['package', '包裹检测', '发现无人看管的静止包裹'],
  ['locate', '地面定位', '地面坐标、轨迹和鸟瞰图']
];
const AUDIO_FEATURES = new Set(['intrusion', 'fall', 'fire', 'fence', 'dwell']);
const REGION_BINDINGS = [
  ['intrusion', '入侵检测'], ['fence', '电子围栏'], ['dwell', '人员滞留'], ['locate', '地面定位']
];
const LINE_BINDINGS = [['count', '人数统计']];
const REGION_FEATURE_KEYS = new Set(REGION_BINDINGS.map(([key]) => key));
const ROLE_NAMES = {private: '自家区域', public: '公共区域', ignore: '忽略区', ground: '地面标定', '': '未标注'};
const LEVEL_NAMES = {critical: '紧急', warning: '告警', info: '信息'};
const KIND_NAMES = {
  enter: '进入', exit: '离开', cross: '越线', stay: '滞留', stay_long: '超长滞留',
  stay_end: '滞留结束', abandoned: '遗留', removed: '取走', fall: '跌倒',
  position: '位置', direction: '方向', fire: '火焰', smoke: '烟雾',
  zone_enter: '进入地面区域', zone_exit: '离开地面区域', dwell: '区域停留', clear: '解除'
};
const FEATURE_NAMES = Object.fromEntries(FEATURES);
const COLORS = ['#30d158','#ffb020','#00a5ff','#af7aff','#ff6b6b','#00d5c8','#d4e157','#ff9ed2','#bcaaa4','#90caf9','#ff8a50','#b2df8a'];
const MAX_REGIONS = COLORS.length;
const GEO_PROVIDERS = [
  ['wifi_google', 'Wi-Fi 定位（Google）'],
  ['ip', 'IP 定位'],
  ['manual', '手动填写']
];

const S = {
  view: 'live',
  connected: false,
  config: {},
  status: {},
  ground: {},
  geo: {},
  dwell: {},
  heat: null,
  events: [],
  eventTotal: 0,
  eventCursor: null,
  eventLevel: '',
  eventPages: 0,
  selectedEvents: new Set(),
  regions: {},
  lines: {},
  groundZones: {},
  selectedId: null,
  dirty: false,
  drawMode: null,
  draft: [],
  drag: null,
  editorImageReady: false,
  showHeatmap: false,
  saving: false,
  regionView: null,
  regionViewKey: '',
  polling: {},
  lastStatusAt: 0,
  lastPowerMode: null,
  language: 'zh_CN',
  languageHoldUntil: 0,
  geoLocating: false
};

function getPath(obj, path, fallback) {
  let cur = obj;
  for (const key of String(path).split('.')) {
    if (cur == null || typeof cur !== 'object' || !(key in cur)) return fallback;
    cur = cur[key];
  }
  return cur;
}

function setPath(obj, path, value) {
  const keys = String(path).split('.');
  let cur = obj;
  for (let i = 0; i < keys.length - 1; i++) {
    if (!cur[keys[i]] || typeof cur[keys[i]] !== 'object' || Array.isArray(cur[keys[i]])) cur[keys[i]] = {};
    cur = cur[keys[i]];
  }
  cur[keys[keys.length - 1]] = value;
  return obj;
}

function deepMerge(base, over) {
  if (!base || typeof base !== 'object' || Array.isArray(base) ||
      !over || typeof over !== 'object' || Array.isArray(over)) return clone(over);
  const out = clone(base);
  for (const [key, value] of Object.entries(over)) {
    out[key] = out[key] && typeof out[key] === 'object' && !Array.isArray(out[key]) &&
      value && typeof value === 'object' && !Array.isArray(value)
      ? deepMerge(out[key], value) : clone(value);
  }
  return out;
}

function levelBadge(level) {
  return `<span class="level ${esc(level || 'info')}">${esc(translateExact(LEVEL_NAMES[level] || level || '信息'))}</span>`;
}

function featureText(e) {
  const feature = FEATURE_NAMES[e.feature] || e.feature || '事件';
  const kind = KIND_NAMES[e.kind] || e.kind || '';
  return translateExact(kind ? `${feature} / ${kind}` : feature);
}

function targetText(e) {
  const label = e.label || e.class_name || e.name || '';
  const id = e.track_id != null ? `#${e.track_id}` : '';
  const extra = e.extra ? ` · ${e.extra}` : '';
  return `${label} ${id}${extra}`.trim();
}

function toast(message, type = '') {
  const stack = $('#toast-stack');
  const item = document.createElement('div');
  item.className = `toast ${type}`;
  item.textContent = message;
  stack.appendChild(item);
  setTimeout(() => item.remove(), 4200);
}

function setConnected(ok, text) {
  S.connected = ok;
  $('#conn-dot').className = `dot ${ok ? 'online' : 'offline'}`;
  $('#conn-text').textContent = text || (ok ? '服务在线' : '连接断开');
}

async function api(path, options = {}) {
  const opts = {cache: 'no-store', ...options};
  if (opts.body != null && typeof opts.body !== 'string') {
    opts.headers = {'Content-Type': 'application/json', ...(opts.headers || {})};
    opts.body = JSON.stringify(opts.body);
  }
  let res;
  try {
    res = await fetch(path, opts);
  } catch (err) {
    setConnected(false, '连接断开');
    throw new Error(`无法连接检测服务: ${err.message || err}`);
  }
  let data = null;
  const text = await res.text();
  if (text) {
    try { data = JSON.parse(text); } catch (_) { data = text; }
  }
  if (data && typeof data === 'object' && data.ok === false && data.error) {
    throw new Error(data.error);
  }
  if (!res.ok) {
    const msg = data && typeof data === 'object' ? data.error : text;
    throw new Error(msg || `HTTP ${res.status}`);
  }
  setConnected(true, '服务在线');
  return data;
}

function withBusy(name, fn) {
  if (S.polling[name]) return Promise.resolve();
  S.polling[name] = true;
  return Promise.resolve().then(fn).finally(() => { S.polling[name] = false; });
}

function showView(name, options = {}) {
  if (!VIEW_META[name]) return;
  S.view = name;
  document.body.dataset.view = name;
  document.documentElement.dataset.view = name;
  $$('.nav-btn').forEach(b => b.classList.toggle('active', b.dataset.view === name));
  $$('.view').forEach(v => v.classList.toggle('active', v.id === `view-${name}`));
  $('#page-title').textContent = VIEW_META[name][0];
  $('#page-subtitle').textContent = VIEW_META[name][1];
  if (name === 'events') loadEvents(true);
  if (name === 'regions') ensureRegionEditor();
  if (name === 'ground') loadGround();
  if (name === 'settings') applyConfigForm();
  if (name === 'status' || name === 'live') loadStatus();
  if (name === 'ground') requestAnimationFrame(renderBev);
  if (name === 'regions') requestAnimationFrame(renderRegionCanvas);
  if (!options.keepScroll) window.scrollTo({top: 0, behavior: 'instant'});
}

function bindNavigation() {
  $$('.nav-btn').forEach(btn => btn.addEventListener('click', () => showView(btn.dataset.view)));
  $$('[data-goto]').forEach(btn => btn.addEventListener('click', () => showView(btn.dataset.goto)));
  $$('.settings-link').forEach(btn => btn.addEventListener('click', () => {
    $$('.settings-link').forEach(x => x.classList.toggle('active', x === btn));
    $$('.settings-pane').forEach(x => x.classList.toggle('active', x.dataset.pane === btn.dataset.settings));
    if (btn.dataset.settings === 'advanced') {
      $('#advanced-json').value = JSON.stringify(S.config, null, 2);
    }
  }));
  $$('[data-close-modal]').forEach(btn => btn.addEventListener('click', () => {
    $(`#${btn.dataset.closeModal}`).classList.add('hidden');
  }));
  $$('.modal').forEach(modal => modal.addEventListener('click', e => {
    if (e.target === modal) modal.classList.add('hidden');
  }));
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') {
      $$('.modal').forEach(m => m.classList.add('hidden'));
      if (S.drawMode) cancelDrawing();
    }
    const editingText = document.activeElement?.matches(
      'input, textarea, select, [contenteditable="true"]');
    const nudge = {ArrowUp: [0, -1], ArrowDown: [0, 1], ArrowLeft: [-1, 0], ArrowRight: [1, 0]}[e.key];
    if (nudge && S.view === 'regions' && S.selectedId && !S.drawMode && !editingText) {
      e.preventDefault();
      const step = e.shiftKey ? .01 : .0025;
      nudgeSelected(nudge[0] * step, nudge[1] * step);
      return;
    }
    if ((e.key === 'Delete' || e.key === 'Backspace') && S.view === 'regions' &&
        S.selectedId && !editingText) {
      e.preventDefault();
      deleteSelectedRegion();
    }
  });
}

async function loadStatus() {
  return withBusy('status', async () => {
    try {
      const st = await api('/status');
      S.status = st || {};
      S.lastStatusAt = Date.now();
      renderStatus();
      if (!S.events.length) loadEvents(false);
    } catch (err) {
      setConnected(false, '连接断开');
      $('#live-state').textContent = '离线';
      $('#stream-hud').textContent = err.message;
    }
  });
}

function renderStatus() {
  const st = S.status || {};
  const camera = st.camera || {};
  const power = st.power || {};
  const h264 = st.h264 || {};
  const mode = power.mode || 'normal';
  const standby = mode === 'standby' || power.camera_on === false;
  const recovered = S.lastPowerMode === 'standby' && mode !== 'standby';
  S.lastPowerMode = mode;
  if (st.app_language && Date.now() > (S.languageHoldUntil || 0)) {
    const reportedLanguage = normalizeLanguage(st.app_language);
    if (reportedLanguage !== currentLanguage) {
      setLanguage(reportedLanguage, {rerender: false});
      setTimeout(rerenderLocalizedUi, 0);
    }
  }
  setConnected(true, '服务在线');
  $('#live-state').textContent = standby ? '待机' : (camera.connected === false ? '摄像头断开' : '在线');
  $('#standby-overlay').classList.toggle('hidden', !standby);
  $('#live-stream').style.visibility = standby ? 'hidden' : 'visible';
  $('#m-capture').textContent = fmt(st.capture_fps, 1);
  $('#m-ai').textContent = fmt(st.ai_ms, 1);
  $('#m-tracks').textContent = String(st.tracks ?? '—');
  $('#m-latency').textContent = fmt(st.latency_ms, 0);
  $('#live-caption').textContent = `MJPEG · ${st.mjpeg?.width || 640}×${st.mjpeg?.height || 360} · ${st.mjpeg?.fps || 10} fps`;
  $('#stream-hud').textContent = standby ? translateExact('摄像头已关闭，Web 与事件查询仍可用') :
    currentLanguage === 'en'
      ? `Capture ${fmt(st.capture_fps, 1)} fps · AI ${fmt(st.ai_ms, 1)} ms · Latency ${fmt(st.latency_ms, 0)} ms · Tracks ${st.tracks ?? 0}`
      : `采集 ${fmt(st.capture_fps, 1)} fps · AI ${fmt(st.ai_ms, 1)} ms · 延迟 ${fmt(st.latency_ms, 0)} ms · 跟踪 ${st.tracks ?? 0}`;
  $('#camera-state').textContent = standby ? '待机关闭' : (camera.connected ? '已连接' : '断开');
  $('#camera-size').textContent = `${camera.width || 0}×${camera.height || 0} ${camera.format || ''}`.trim();
  $('#infer-state').textContent = st.detector_running ? `${fmt(st.infer_fps ?? st.ai_fps, 0)} fps` : '未运行';
  $('#h264-state').textContent = h264.running ? `${h264.width || 0}×${h264.height || 0} · ${h264.bitrate_kbps || 0} kbps` : '未运行';
  $('#event-count').textContent = `${st.events?.stored ?? 0} 条`;
  $('#power-badge').textContent = mode === 'normal' ? '正常' : mode === 'eco' ? '节能' : '待机';
  if ($('#power-mode').value !== mode) $('#power-mode').value = mode;
  $('#power-apply').textContent = standby ? '打开摄像头' : '应用模式';

  const counts = st.line_counts || {};
  const lines = Object.entries(counts);
  $('#counts-list').innerHTML = lines.length ? lines.map(([id, c]) => `
    <div class="simple-row"><span><strong>${esc(id)}</strong><br><small>IN ${c.in ?? 0} · OUT ${c.out ?? 0}</small></span><em>净 ${c.net ?? 0}</em></div>
  `).join('') : '<span class="muted">暂无计数线</span>';

  renderRecentEvents();
  renderViewAlert(st.view || {});
  renderHeatOverlay();

  const alerts = [];
  if (camera.connected === false && !standby) alerts.push('摄像头已断开，mb-capture 正在重连。');
  const globalAlert = $('#global-alert');
  if (alerts.length) {
    globalAlert.textContent = alerts.join(' ');
    globalAlert.classList.remove('hidden');
  } else globalAlert.classList.add('hidden');

  renderStatusPage();
  renderGeoSummary();
  syncRegionViewFromStatus();
  if (recovered && S.view === 'regions') loadEditorImage();
  if (S.view === 'live' || S.view === 'ground') document.documentElement.scrollTop = 0;
}

function renderViewAlert(view) {
  const notice = view.notice || '';
  const visible = notice && view.state !== 'ok';
  $('#view-alert').classList.toggle('hidden', !visible);
  if (visible) $('#view-alert-text').textContent = notice;
}

function renderRecentEvents() {
  const box = $('#recent-events');
  const list = S.events.slice(0, 4);
  box.innerHTML = list.length ? list.map(e => `
    <div class="event-mini" data-event-id="${esc(e.id)}">
      <time>${esc(shortTime(e.time))}</time>
      <span><strong>${esc(featureText(e))}</strong><small>${esc(targetText(e))}</small></span>
    </div>
  `).join('') : '<span class="muted">暂无事件</span>';
  $$('.event-mini', box).forEach(row => row.addEventListener('click', () => openEvent(row.dataset.eventId)));
}

function renderStatusPage() {
  const st = S.status || {};
  const camera = st.camera || {};
  const h264 = st.h264 || {};
  const events = st.events || {};
  const view = st.view || {};
  const dwell = st.dwell || {};
  const age = S.lastStatusAt ? Math.max(0, Math.round((Date.now() - S.lastStatusAt) / 1000)) : null;
  $('#status-age').textContent = age == null ? '—' : `${age} 秒前`;
  $('#status-details').innerHTML = details([
    ['运行时长', `${fmt(st.uptime_sec, 0)} 秒`],
    ['采集帧率', `${fmt(st.capture_fps, 2)} fps`],
    ['AI 耗时', `${fmt(st.ai_ms, 2)} ms`],
    ['解码耗时', `${fmt(st.decode_ms, 2)} ms`],
    ['处理延迟', `${fmt(st.latency_ms, 1)} ms`],
    ['摄像头', `${camera.device || '—'} · ${camera.connected ? '已连接' : '断开'}`],
    ['重连次数', String(camera.reconnects ?? 0)],
    ['目标 / 轨迹', `${st.detections ?? 0} / ${st.tracks ?? 0}`]
  ]);
  $('#recording-stats').innerHTML = details([
    ['已保存事件', String(events.stored ?? 0)],
    ['已触发', String(events.fired ?? 0)],
    ['H.264', h264.running ? '运行中' : '未运行'],
    ['编码帧', String(h264.encoded ?? 0)],
    ['丢弃帧', String(h264.dropped ?? 0)],
    ['录像成功', String(h264.clips_ok ?? 0)],
    ['录像失败', String(h264.clips_failed ?? 0)],
    ['UDP 包', String(h264.udp?.packets ?? 0)]
  ]);
  $('#view-stats').innerHTML = details([
    ['状态', translateExact(view.state === 'ok' ? '正常' : (view.state || '—'))],
    ['自动跟随', view.auto_correct ? '开启' : '关闭'],
    ['引用图', String(view.references ?? 0)],
    ['平移', view.shift ? `${fmt(view.shift.dx, 4)}, ${fmt(view.shift.dy, 4)}` : '—'],
    ['旋转', view.shift ? `${fmt(view.shift.rotation_deg, 2)}°` : '—'],
    ['缩放', view.shift ? fmt(view.shift.scale, 4) : '—'],
    ['置信度', view.last ? fmt(view.last.confident, 2) : '—'],
    ['内点', String(view.last?.inliers ?? '—')]
  ]);
  $('#dwell-stats').innerHTML = details([
    ['正在滞留', String(dwell.staying ?? 0)],
    ['计时中', String(dwell.counting ?? 0)],
    ['最长停留', `${fmt(dwell.longest_sec, 0)} 秒`],
    ['会话数', String(dwell.sessions ?? 0)],
    ['告警数', String(dwell.warnings ?? 0)],
    ['喊话后离开', String(dwell.left_after_warning ?? 0)]
  ]);
  $('#config-stats').innerHTML = details([
    ['配置路径', st.config || '—'],
    ['事件目录', events.dir || '—'],
    ['Web 端口', String(st.mjpeg ? location.port : '—')],
    ['MJPEG 客户端', String(st.mjpeg?.clients ?? 0)],
    ['已加载火焰模型', st.fire_loaded ? '是' : '否'],
    ['已加载姿态模型', st.pose_loaded ? '是' : '否']
  ]);
}

function details(rows) {
  return rows.map(([k, v]) => `<div><dt>${esc(k)}</dt><dd>${esc(v)}</dd></div>`).join('');
}

async function loadEvents(reset = false, older = false) {
  return withBusy('events', async () => {
    try {
      if (reset) {
        S.eventPages = 0;
        S.eventCursor = null;
      }
      const level = $('#event-level')?.value || '';
      S.eventLevel = level;
      let path = `/events?limit=50${level ? `&level=${encodeURIComponent(level)}` : ''}`;
      if (older && S.eventCursor) path += `&before=${encodeURIComponent(S.eventCursor)}`;
      const data = await api(path);
      const incoming = Array.isArray(data.events) ? data.events : [];
      S.eventTotal = data.total ?? incoming.length;
      if (reset || !older) {
        S.events = incoming;
        S.eventPages = 0;
      } else {
        const seen = new Set(S.events.map(e => e.id));
        S.events.push(...incoming.filter(e => !seen.has(e.id)));
        S.eventPages += 1;
      }
      S.eventCursor = S.events.length ? S.events[S.events.length - 1].id : null;
      renderEvents();
      renderRecentEvents();
      renderStatus();
    } catch (err) {
      $('#events-body').innerHTML = `<tr><td colspan="8" class="empty-row">${esc(err.message)}</td></tr>`;
      toast(err.message, 'error');
    }
  });
}

function renderEvents() {
  const body = $('#events-body');
  if (!S.events.length) {
    body.innerHTML = '<tr><td colspan="8" class="empty-row">暂无符合条件的事件</td></tr>';
  } else {
    body.innerHTML = S.events.map(e => {
      const checked = S.selectedEvents.has(e.id) ? 'checked' : '';
      const snapshot = e.snapshot ? `<a href="/events/${encodeURIComponent(e.id)}.jpg" target="_blank"><img class="thumb" loading="lazy" src="/events/${encodeURIComponent(e.id)}.jpg" alt="快照"></a>` : '<span class="muted">无</span>';
      let clip = '<span class="muted">无</span>';
      if (e.clip_state === 'ready') clip = `<a href="/events/${encodeURIComponent(e.id)}.mp4" target="_blank">回放</a> <button class="btn tiny ghost" data-del-clip="${esc(e.id)}">删录像</button>`;
      else if (e.clip_state === 'recording') clip = '<span class="muted">生成中</span>';
      else if (e.clip_state === 'deleted') clip = '<span class="muted">已删除</span>';
      else if (e.clip_state === 'failed') clip = '<span class="muted">失败</span>';
      return `<tr>
        <td><input type="checkbox" data-event-select="${esc(e.id)}" ${checked} aria-label="选择事件"></td>
        <td>${esc(timeText(e.time))}</td>
        <td>${levelBadge(e.level)}</td>
        <td>${esc(featureText(e))}</td>
        <td>${esc(targetText(e))}</td>
        <td>${snapshot}</td>
        <td>${clip}</td>
        <td><div class="row-actions"><button class="btn tiny secondary" data-open-event="${esc(e.id)}">详情</button><button class="btn tiny danger" data-del-event="${esc(e.id)}">删除</button></div></td>
      </tr>`;
    }).join('');
  }
  $('#events-summary').textContent = translatePhrase(`共 ${S.eventTotal} 条 · 当前显示 ${S.events.length} 条`);
  $('#events-page-label').textContent = translatePhrase(S.eventPages ? `已加载 ${S.eventPages + 1} 批` : '第 1 批');
  $('#events-newer-page').disabled = !S.eventPages;
  $('#events-older-page').disabled = S.events.length >= S.eventTotal;
  updateEventSelection();
  $$('[data-event-select]', body).forEach(cb => cb.addEventListener('change', () => {
    cb.checked ? S.selectedEvents.add(cb.dataset.eventSelect) : S.selectedEvents.delete(cb.dataset.eventSelect);
    updateEventSelection();
  }));
  $$('[data-open-event]', body).forEach(btn => btn.addEventListener('click', () => openEvent(btn.dataset.openEvent)));
  $$('[data-del-event]', body).forEach(btn => btn.addEventListener('click', () => deleteEvent(btn.dataset.delEvent, false)));
  $$('[data-del-clip]', body).forEach(btn => btn.addEventListener('click', () => deleteEvent(btn.dataset.delClip, true)));
}

function updateEventSelection() {
  const visible = new Set(S.events.map(e => e.id));
  for (const id of [...S.selectedEvents]) if (!visible.has(id)) S.selectedEvents.delete(id);
  const btn = $('#events-delete');
  btn.disabled = S.selectedEvents.size === 0;
  btn.textContent = S.selectedEvents.size ? `删除所选 (${S.selectedEvents.size})` : '删除所选';
  const all = $('#events-all');
  all.checked = S.events.length > 0 && S.events.every(e => S.selectedEvents.has(e.id));
  all.indeterminate = S.selectedEvents.size > 0 && !all.checked;
}

async function deleteEvent(id, clipOnly) {
  const question = clipOnly ? '只删除这条事件的录像，保留记录和快照？' : '删除这条事件及其快照、录像？';
  if (!confirm(question)) return;
  try {
    await api(`/events/${encodeURIComponent(id)}${clipOnly ? '.mp4' : ''}`, {method: 'DELETE'});
    toast(clipOnly ? '录像已删除' : '事件已删除', 'ok');
    S.selectedEvents.delete(id);
    await loadEvents(true);
  } catch (err) { toast(`删除失败: ${err.message}`, 'error'); }
}

async function bulkDeleteEvents() {
  const ids = [...S.selectedEvents];
  if (!ids.length) return;
  if (!confirm(`删除选中的 ${ids.length} 条事件及其快照、录像？录像生成中的事件会跳过。`)) return;
  try {
    const data = await api('/events/delete', {method: 'POST', body: {ids}});
    for (const id of data.deleted || []) S.selectedEvents.delete(id);
    let message = `已删除 ${data.deleted?.length || 0} 条`;
    if (data.busy?.length) message += `，${data.busy.length} 条正在录像`;
    if (data.failed?.length) message += `，${data.failed.length} 条失败`;
    toast(message, data.ok ? 'ok' : 'error');
    await loadEvents(true);
  } catch (err) { toast(`批量删除失败: ${err.message}`, 'error'); }
}

async function openEvent(id) {
  const modal = $('#event-modal');
  const box = $('#event-modal-body');
  box.innerHTML = '<p class="muted">正在读取事件…</p>';
  modal.classList.remove('hidden');
  try {
    const e = await api(`/events/${encodeURIComponent(id)}`);
    const media = e.snapshot || e.clip_state === 'ready'
      ? `<div class="media-stack">${e.clip_state === 'ready' ? `<video controls playsinline src="/events/${encodeURIComponent(id)}.mp4"></video>` : ''}${e.snapshot ? `<img src="/events/${encodeURIComponent(id)}.jpg" alt="事件快照">` : ''}</div>`
      : '<div class="empty-row">这条事件没有可用媒体</div>';
    box.innerHTML = `<div class="event-detail-grid">
      <div>${media}<div class="modal-actions">
        ${e.clip_state === 'ready' ? `<a class="btn secondary" href="/events/${encodeURIComponent(id)}.mp4" target="_blank">单独打开录像</a>` : ''}
        ${e.snapshot ? `<a class="btn secondary" href="/events/${encodeURIComponent(id)}.jpg" target="_blank">查看原图</a>` : ''}
        ${e.clip_state === 'ready' ? `<button class="btn ghost" data-modal-del-clip="${esc(id)}">删除录像</button>` : ''}
        <button class="btn danger" data-modal-del-event="${esc(id)}">删除事件</button>
      </div></div>
      <div><h2 id="event-modal-title">${esc(featureText(e))}</h2><p class="muted" style="margin:4px 0 14px">${esc(timeText(e.time))}</p>
      <div class="details-list">${details([
        ['级别', LEVEL_NAMES[e.level] || e.level || '—'],
        ['事件 ID', e.id || id],
        ['区域', e.name || '—'],
        ['目标', targetText(e)],
        ['录像', ({ready:'就绪',recording:'生成中',failed:'失败',deleted:'已删除'})[e.clip_state] || '无'],
        ['附加信息', e.extra || '—']
      ])}</div>
      <details style="margin-top:14px"><summary class="muted">查看原始 JSON</summary><pre class="json-view">${esc(JSON.stringify(e, null, 2))}</pre></details>
      </div></div>`;
    $('[data-modal-del-clip]', box)?.addEventListener('click', async () => {
      await deleteEvent(id, true);
      modal.classList.add('hidden');
    });
    $('[data-modal-del-event]', box)?.addEventListener('click', async () => {
      await deleteEvent(id, false);
      modal.classList.add('hidden');
    });
  } catch (err) {
    box.innerHTML = `<p class="empty-row">${esc(err.message)}</p>`;
  }
}

function applyTheme(theme) {
  const dark = theme === 'dark';
  document.body.dataset.theme = dark ? 'dark' : 'light';
  try { localStorage.setItem('monitor-box-theme', dark ? 'dark' : 'light'); } catch (_) {}
  const btn = $('#theme-toggle');
  if (btn) {
    btn.textContent = dark ? '浅色' : '深色';
    btn.setAttribute('aria-label', dark ? '切换到浅色主题' : '切换到深色主题');
    btn.setAttribute('aria-pressed', dark ? 'true' : 'false');
  }
}

function bindStatusActions() {
  $('#web-language').addEventListener('change', e => changeLanguage(e.target.value));
  $('#theme-toggle').addEventListener('click', () => {
    applyTheme(document.body.dataset.theme === 'dark' ? 'light' : 'dark');
    renderRegionCanvas();
    renderHeatOverlay();
    if (S.view === 'ground') renderBev();
  });
  $('#refresh-all').addEventListener('click', () => refreshEverything(true));
  $('#power-apply').addEventListener('click', async () => {
    const standbyNow = S.status?.power?.mode === 'standby';
    if (standbyNow) {
      try {
        await api('/power', {method: 'POST', body: {mode: 'normal'}});
        toast('摄像头已打开', 'ok');
        setTimeout(loadStatus, 700);
        setTimeout(() => { if (S.view === 'regions') loadEditorImage(); }, 2200);
      } catch (err) { toast(`打开摄像头失败: ${err.message}`, 'error'); }
      return;
    }
    const current = S.status?.power?.mode || 'normal';
    let mode = $('#power-mode').value;
    if (current !== 'standby' && mode === 'standby' && !confirm('关闭摄像头进入待机？待机期间不检测、不告警、不录像。')) return;
    if (current === 'standby' && mode === 'standby') mode = 'normal';
    try {
      await api('/power', {method: 'POST', body: {mode}});
      toast(mode === 'standby' ? '摄像头已关闭，设备进入待机' : '功耗模式已切换', 'ok');
      setTimeout(loadStatus, 700);
      setTimeout(loadConfig, 1200);
    } catch (err) { toast(`切换失败: ${err.message}`, 'error'); }
  });
  $('#reset-counts').addEventListener('click', async () => {
    if (!confirm('清零所有越线计数？历史 counts.jsonl 不受影响。')) return;
    try { await api('/counts', {method: 'DELETE'}); toast('越线计数已清零', 'ok'); loadStatus(); }
    catch (err) { toast(`清零失败: ${err.message}`, 'error'); }
  });
  $$('[data-view-action]').forEach(btn => btn.addEventListener('click', async () => {
    try {
      await api('/view', {method: 'POST', body: {action: btn.dataset.viewAction}});
      toast('画面处理已更新', 'ok');
      loadStatus();
    } catch (err) { toast(`操作失败: ${err.message}`, 'error'); }
  }));
  $('#event-level').addEventListener('change', () => loadEvents(true));
  $('#events-refresh').addEventListener('click', () => loadEvents(true));
  $('#events-delete').addEventListener('click', bulkDeleteEvents);
  $('#events-all').addEventListener('change', e => {
    for (const ev of S.events) e.target.checked ? S.selectedEvents.add(ev.id) : S.selectedEvents.delete(ev.id);
    renderEvents();
  });
  $('#events-older-page').addEventListener('click', () => loadEvents(false, true));
  $('#events-newer-page').addEventListener('click', () => loadEvents(true));
}

async function refreshEverything(showToast = false) {
  if (S.dirty && !confirm('区域修改尚未保存，刷新会丢弃这些修改。继续刷新？')) return;
  await Promise.allSettled([loadStatus(), loadEvents(true), loadConfig(true), loadGround(), loadGeo()]);
  if (showToast) toast('数据已刷新', 'ok');
}

async function loadGeo() {
  try {
    S.geo = await api('/geo');
    renderGeoSummary();
  } catch (_) { /* /geo may be disabled */ }
}

function geoText(geo = S.geo) {
  if (!geo || geo.valid === false) return geo?.error || '尚未定位';
  if (typeof geo.lat !== 'number' || typeof geo.lon !== 'number') return '尚未定位';
  const where = geo.address ? ` · ${geo.address}` : '';
  return `${geo.lat.toFixed(6)}, ${geo.lon.toFixed(6)} · ${geo.source || '未知来源'} · ±${fmt(geo.accuracy_m, 0)} m${where}`;
}

function renderGeoSummary() {
  const geo = S.geo || S.status?.geo || {};
  const box = $('#geo-summary');
  if (box) box.innerHTML = details([
    ['状态', S.geoLocating ? '定位中' : geo.enabled === false ? '已关闭' : geo.valid ? '已定位' : '尚未定位'],
    ['经纬度', geo.valid ? `${fmt(geo.lat, 6)}, ${fmt(geo.lon, 6)}` : '—'],
    ['来源', geo.source || '—'],
    ['精度', geo.accuracy_m != null ? `±${fmt(geo.accuracy_m, 0)} 米` : '—'],
    ['地址', geo.address || '—'],
    ['更新时间', geo.updated || '—']
  ]);
  const button = $('#geo-refresh');
  if (button) {
    button.disabled = S.geoLocating;
    button.textContent = translateExact(S.geoLocating ? '定位中…' : '立即定位');
  }
}

async function loadConfig(force = false) {
  try {
    const cfg = await api('/config');
    if (S.dirty && !force) return S.config;
    S.config = cfg || {};
    const configuredLanguage = normalizeLanguage(getPath(S.config, 'app.language', 'zh_CN'));
    if (configuredLanguage !== currentLanguage) setLanguage(configuredLanguage, {rerender: false});
    hydrateRegionState(S.config);
    applyConfigForm();
    renderRegionEditor();
    renderGround();
    return S.config;
  } catch (err) {
    toast(`读取配置失败: ${err.message}`, 'error');
    return S.config;
  }
}

async function saveConfigPatch(patch, options = {}) {
  const current = await api('/config');
  const next = deepMerge(current, patch);
  const res = await api('/config', {method: 'PUT', body: next});
  const need = Array.isArray(res.restart_required) ? res.restart_required : [];
  S.config = next;
  if (!options.keepRegions && !S.dirty) {
    hydrateRegionState(next);
    renderRegionEditor();
  }
  applyConfigForm();
  renderGround();
  let message = options.message || '配置已保存';
  if (need.length) message += `；需重启 mb-capture 生效：${need.join(', ')}`;
  toast(message, 'ok');
  return res;
}

async function putFullConfig(config, message = '配置已保存') {
  const res = await api('/config', {method: 'PUT', body: config});
  const need = Array.isArray(res.restart_required) ? res.restart_required : [];
  S.config = config;
  hydrateRegionState(config);
  applyConfigForm();
  renderRegionEditor();
  renderGround();
  toast(need.length ? `${message}；需重启：${need.join(', ')}` : message, 'ok');
  return res;
}

function applyConfigForm() {
  const cfg = S.config || {};
  const configLanguage = normalizeLanguage(getPath(cfg, 'app.language', currentLanguage));
  if (configLanguage !== currentLanguage) setLanguage(configLanguage, {rerender: false});
  $$('[data-cfg]').forEach(input => {
    const useDefault = input.dataset.default;
    let value = getPath(cfg, input.dataset.cfg, undefined);
    if (value === undefined && useDefault !== undefined) value = useDefault;
    if (input.type === 'checkbox') input.checked = Boolean(value);
    else input.value = value == null ? '' : String(value);
  });
  const geo = cfg.geo || {};
  const manual = geo.manual || {};
  if ($('#geo-lat')) $('#geo-lat').value = manual.lat == null ? '' : manual.lat;
  if ($('#geo-lon')) $('#geo-lon').value = manual.lon == null ? '' : manual.lon;
  if ($('#geo-accuracy')) $('#geo-accuracy').value = manual.accuracy_m ?? 50;
  if ($('#geo-address')) $('#geo-address').value = manual.address || '';
  renderFeatureSwitches();
  renderGeoProviders();
  if ($('#advanced-json') && $('.settings-pane[data-pane=advanced]').classList.contains('active')) {
    $('#advanced-json').value = JSON.stringify(cfg, null, 2);
  }
}

function featureEnabled(key) {
  return getPath(S.config, `features.${key}.enabled`, false) === true;
}

function featureBoundCount(key) {
  if (key === 'count') return Object.values(S.lines).filter(l => ruleEnabled(l, key)).length;
  if (['intrusion', 'fence', 'dwell', 'locate'].includes(key)) {
    return Object.entries(S.regions).filter(([, r]) => {
      const role = r.role || '';
      if (key === 'locate') return role === 'ground' && ruleEnabled(r, key);
      return ['private','public',''].includes(role) && ruleEnabled(r, key);
    }).length;
  }
  return 0;
}

function audioEnabled(key) {
  const speaker = getPath(S.config, `features.${key}.actions.speaker.enabled`, undefined);
  return speaker === undefined ? AUDIO_FEATURES.has(key) : Boolean(speaker);
}

function renderFeatureSwitches() {
  const box = $('#feature-switches');
  if (!box) return;
  box.innerHTML = FEATURES.map(([key, name, desc]) => `
    <label class="toggle-row"><span><strong>${esc(translateExact(name))}</strong><small>${esc(translateExact(desc))}</small></span>
      <input type="checkbox" data-feature="${key}" ${featureEnabled(key) ? 'checked' : ''}></label>`).join('');
  $$('[data-feature]', box).forEach(cb => cb.addEventListener('change', () => toggleFeature(cb.dataset.feature, cb.checked, cb)));
  const audio = $('#audio-switches');
  if (audio) {
    audio.innerHTML = FEATURES.filter(([key]) => AUDIO_FEATURES.has(key)).map(([key, name]) => `
      <label class="toggle-row"><span><strong>${esc(name)}</strong><small>事件确认后播放对应等级的声音</small></span>
        <input type="checkbox" data-audio="${key}" ${audioEnabled(key) ? 'checked' : ''}></label>`).join('');
    $$('[data-audio]', audio).forEach(cb => cb.addEventListener('change', async () => {
      cb.disabled = true;
      try {
        await saveConfigPatch({features: {[cb.dataset.audio]: {actions: {speaker: {enabled: cb.checked}}}}},
          {message: '事件音频设置已保存'});
      } catch (err) {
        cb.checked = !cb.checked;
        toast(err.message, 'error');
      } finally { cb.disabled = false; }
    }));
  }
}

async function toggleFeature(key, enabled, input) {
  if (enabled && ['intrusion','fence','dwell','count','locate'].includes(key) && featureBoundCount(key) === 0) {
    input.checked = false;
    const where = key === 'count' ? '至少一条进出线' : '至少一个匹配用途的区域';
    if (confirm(`${FEATURE_NAMES[key]}开启前需要绑定${where}。现在前往区域设置？`)) showView('regions');
    return;
  }
  input.disabled = true;
  try {
    await saveConfigPatch({features: {[key]: {enabled}}}, {message: `${FEATURE_NAMES[key]}已${enabled ? '开启' : '关闭'}`});
    if (key === 'locate') renderGround();
  } catch (err) {
    input.checked = !enabled;
    toast(err.message, 'error');
  } finally { input.disabled = false; }
}

function renderGeoProviders() {
  const box = $('#geo-providers');
  if (!box) return;
  const providers = Array.isArray(S.config?.geo?.providers) ? S.config.geo.providers : GEO_PROVIDERS.map(x => x[0]);
  const rows = [...providers.filter(k => GEO_PROVIDERS.some(p => p[0] === k)),
    ...GEO_PROVIDERS.map(p => p[0]).filter(k => !providers.includes(k))];
  box.innerHTML = `<h3>定位来源与顺序</h3><div class="provider-list">${rows.map((key, i) => {
    const name = GEO_PROVIDERS.find(p => p[0] === key)?.[1] || key;
    const enabled = providers.includes(key);
    return `<div class="provider-row"><label><input type="checkbox" data-geo-provider="${key}" ${enabled ? 'checked' : ''}> ${esc(name)}</label>
      <span><button class="btn tiny ghost" data-geo-up="${key}" ${i === 0 ? 'disabled' : ''}>上移</button>
      <button class="btn tiny ghost" data-geo-down="${key}" ${i === rows.length - 1 ? 'disabled' : ''}>下移</button></span></div>`;
  }).join('')}</div>`;
  $$('[data-geo-provider]', box).forEach(cb => cb.addEventListener('change', () => saveGeoProviders(box)));
  $$('[data-geo-up]', box).forEach(btn => btn.addEventListener('click', () => moveGeoProvider(box, btn.dataset.geoUp, -1)));
  $$('[data-geo-down]', box).forEach(btn => btn.addEventListener('click', () => moveGeoProvider(box, btn.dataset.geoDown, 1)));
}

function geoProviderState(box) {
  return $$('.provider-row', box).map(row => {
    const key = $('[data-geo-provider]', row).dataset.geoProvider;
    return [key, $('[data-geo-provider]', row).checked];
  });
}

async function saveGeoProviders(box) {
  const rows = geoProviderState(box);
  if (!rows.some(([, on]) => on)) {
    toast('至少保留一种定位方式；不需要设备经纬度请关闭自动定位', 'error');
    renderGeoProviders();
    return;
  }
  try {
    await saveConfigPatch({geo: {providers: rows.filter(([, on]) => on).map(([key]) => key)}}, {message: '定位方式已保存'});
  } catch (err) { toast(err.message, 'error'); renderGeoProviders(); }
}

async function moveGeoProvider(box, key, delta) {
  const rows = geoProviderState(box);
  const i = rows.findIndex(([k]) => k === key);
  const j = Math.max(0, Math.min(rows.length - 1, i + delta));
  if (i < 0 || i === j) return;
  rows.splice(j, 0, rows.splice(i, 1)[0]);
  const providers = rows.filter(([, on]) => on).map(([k]) => k);
  if (!providers.length) providers.push(key);
  try {
    await saveConfigPatch({geo: {providers}}, {message: '定位顺序已保存'});
  } catch (err) { toast(err.message, 'error'); }
}

function collectPaneConfig(pane) {
  const cfg = {};
  $$('[data-cfg]', pane).forEach(input => {
    let value;
    if (input.type === 'checkbox') value = input.checked;
    else if (input.type === 'number') {
      value = input.value === '' ? null : Number(input.value);
      if (value != null && !Number.isFinite(value)) throw new Error(`${input.previousElementSibling?.textContent || input.dataset.cfg} 不是有效数字`);
    } else value = input.value;
    if (value != null) setPath(cfg, input.dataset.cfg, value);
  });
  return cfg;
}

async function saveSettingsPane(name) {
  const pane = $(`.settings-pane[data-pane="${name}"]`);
  try {
    const patch = collectPaneConfig(pane);
    if (name === 'ground') {
      const lat = $('#geo-lat').value.trim();
      const lon = $('#geo-lon').value.trim();
      let latV = null, lonV = null;
      if (lat || lon) {
        latV = Number(lat); lonV = Number(lon);
        if (!Number.isFinite(latV) || !Number.isFinite(lonV) || latV < -90 || latV > 90 || lonV < -180 || lonV > 180) {
          throw new Error('纬度范围 -90～90，经度范围 -180～180，且需要同时填写');
        }
      }
      patch.geo = patch.geo || {};
      patch.geo.manual = {lat: latV, lon: lonV, accuracy_m: Number($('#geo-accuracy').value || 50), address: $('#geo-address').value.trim()};
    }
    if (name === 'general' && patch.power?.mode) {
      const current = S.status?.power?.mode || 'normal';
      const mode = patch.power.mode;
      if (current !== 'standby' && mode === 'standby' && !confirm('关闭摄像头进入待机？')) return;
      patch.power.resume_mode = mode === 'standby' ? current : mode;
      patch.power.standby_by_app = false;
    }
    await saveConfigPatch(patch, {message: '设置已保存'});
  } catch (err) { toast(err.message, 'error'); }
}

function bindSettings() {
  $('#settings-language').addEventListener('change', e => changeLanguage(e.target.value));
  $('#settings-general-save').addEventListener('click', () => saveSettingsPane('general'));
  $('#settings-detection-save').addEventListener('click', () => saveSettingsPane('detection'));
  $('#settings-video-save').addEventListener('click', () => saveSettingsPane('video'));
  $('#settings-display-save').addEventListener('click', () => saveSettingsPane('display'));
  $('#settings-ground-save').addEventListener('click', () => saveSettingsPane('ground'));
  $('#settings-events-save').addEventListener('click', () => saveSettingsPane('events'));
  $('#advanced-load').addEventListener('click', async () => {
    if (S.dirty && !confirm('区域修改尚未保存，重新载入完整配置会丢弃这些修改。继续？')) return;
    await loadConfig(true);
    $('#advanced-json').value = JSON.stringify(S.config, null, 2);
  });
  $('#advanced-save').addEventListener('click', async () => {
    let cfg;
    try { cfg = JSON.parse($('#advanced-json').value); }
    catch (err) { toast(`JSON 格式错误: ${err.message}`, 'error'); return; }
    if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) { toast('配置必须是 JSON 对象', 'error'); return; }
    if (!confirm('校验并保存完整配置？区域、模型和摄像头配置都可能因此改变。')) return;
    try { await putFullConfig(cfg, '完整配置已保存'); }
    catch (err) { toast(`配置被拒绝: ${err.message}`, 'error'); }
  });
}

// ---------------------------------------------------------------- 区域编辑
function regionBindingAllowed(key, role) {
  if (!REGION_FEATURE_KEYS.has(key)) return false;
  if (key === 'locate') return role === 'ground';
  return ['private', 'public', ''].includes(role);
}

function dropIncompatibleRegionRules(region) {
  if (!region?.rules || typeof region.rules !== 'object' || Array.isArray(region.rules)) return;
  for (const key of Object.keys(region.rules)) {
    if (REGION_FEATURE_KEYS.has(key) && !regionBindingAllowed(key, region.role || '')) {
      delete region.rules[key];
    }
  }
}

function normalizeRegions(config) {
  const regions = clone(config && typeof config.regions === 'object' ? config.regions : {});
  const lines = clone(config && typeof config.lines === 'object' ? config.lines : {});
  const groundZones = clone(config && typeof config.ground_zones === 'object' ? config.ground_zones : {});
  for (const [id, region] of Object.entries(regions)) {
    if (!region || typeof region !== 'object') continue;
    region.name = region.name || id;
    if (!Object.hasOwn(ROLE_NAMES, region.role || '')) {
      region.role = region.real_size_m || region.calib_points ? 'ground' : '';
    }
    if (!Array.isArray(region.points)) region.points = [];
    region.points = region.points.filter(validPoint);
    for (const field of ['calib_points', 'check_points']) {
      if (!Array.isArray(region[field])) continue;
      region[field] = region[field].filter(c => Array.isArray(c) && c.length === 4 &&
        c.every(v => Number.isFinite(Number(v))));
    }
    if (!region.rules || typeof region.rules !== 'object' || Array.isArray(region.rules)) region.rules = {};
    // A role change in an older config may leave bindings that the current UI
    // cannot show or remove. Discard them while hydrating so Save is actionable.
    dropIncompatibleRegionRules(region);
  }
  for (const [id, line] of Object.entries(lines)) {
    if (!line || typeof line !== 'object') continue;
    line.name = line.name || id;
    if (!line.rules || typeof line.rules !== 'object' || Array.isArray(line.rules)) line.rules = {};
  }
  for (const [id, zone] of Object.entries(groundZones)) {
    if (!zone || typeof zone !== 'object') continue;
    zone.name = zone.name || id;
    zone.enabled = zone.enabled !== false;
    zone.alarm_enter = Boolean(zone.alarm_enter);
    zone.dwell_sec = Number(zone.dwell_sec || 0);
    zone.groups = Array.isArray(zone.groups) ? zone.groups : [];
  }
  const features = config?.features || {};
  for (const [key] of REGION_BINDINGS) {
    const feature = features[key] || {};
    const refs = [feature.region_id, ...(Array.isArray(feature.region_ids) ? feature.region_ids : [])]
      .filter(x => typeof x === 'string' && x && regions[x]);
    for (const ref of refs) {
      if (!regionBindingAllowed(key, regions[ref].role || '')) continue;
      const rules = regions[ref].rules;
      if (!Object.hasOwn(rules, key)) rules[key] = {};
    }
  }
  const countFeature = features.count || {};
  for (const ref of Array.isArray(countFeature.line_ids) ? countFeature.line_ids : []) {
    if (lines[ref] && !Object.hasOwn(lines[ref].rules, 'count')) lines[ref].rules.count = {};
  }
  return {regions, lines, groundZones};
}

function affineKey(applied) {
  return Array.isArray(applied) ? applied.map(Number).join(',') : '';
}

function syncRegionViewFromStatus() {
  if (S.dirty || !S.config || !Object.keys(S.config).length) return;
  const applied = S.status?.view?.applied;
  const key = affineKey(applied);
  if (!key || key === S.regionViewKey) return;
  hydrateRegionState(S.config);
  renderRegionEditor();
}

function applyRegionView(state) {
  const applied = S.status?.view?.applied;
  const key = affineKey(applied);
  if (!key || S.regionViewKey === key) return state;
  S.regionViewKey = key;
  if (!isAffineCorrection(applied)) {
    S.regionView = null;
    return state;
  }
  const map = (p) => validPoint(p) ? [
    round4(Number(applied[0]) * p[0] + Number(applied[1]) * p[1] + Number(applied[2])),
    round4(Number(applied[3]) * p[0] + Number(applied[4]) * p[1] + Number(applied[5]))
  ] : p;
  for (const region of Object.values(state.regions)) {
    region.points = (region.points || []).map(map);
    for (const field of ['calib_points', 'check_points']) {
      if (Array.isArray(region[field])) {
        region[field] = region[field].map(c => {
          if (!validPoint(c)) return c;
          const uv = map(c);
          return [uv[0], uv[1], ...c.slice(2)];
        });
      }
    }
    if (region.role === 'ground' && isGroundPoints(region)) updateCalibHull(region);
  }
  for (const line of Object.values(state.lines)) {
    line.p1 = map(line.p1);
    line.p2 = map(line.p2);
  }
  for (const zone of Object.values(state.groundZones)) {
    if (Array.isArray(zone.points_img)) zone.points_img = zone.points_img.map(map);
  }
  S.regionView = applied;
  return state;
}

function isAffineCorrection(applied) {
  if (!Array.isArray(applied) || applied.length !== 6 || !applied.every(Number.isFinite)) return false;
  return Math.abs(applied[0] - 1) > 1e-5 || Math.abs(applied[1]) > 1e-5 ||
    Math.abs(applied[2]) > 1e-5 || Math.abs(applied[3]) > 1e-5 ||
    Math.abs(applied[4] - 1) > 1e-5 || Math.abs(applied[5]) > 1e-5;
}

function hydrateRegionState(config) {
  const state = applyRegionView(normalizeRegions(config || {}));
  S.regions = state.regions;
  S.lines = state.lines;
  S.groundZones = state.groundZones;
  S.dirty = false;
  S.drawMode = null;
  S.draft = [];
  if (S.selectedId && !findRegionItem(S.selectedId)) S.selectedId = null;
  updateSaveButton();
}

function findRegionItem(id = S.selectedId) {
  if (!id) return null;
  if (S.regions[id]) return {id, kind: 'region', item: S.regions[id]};
  if (S.lines[id]) return {id, kind: 'line', item: S.lines[id]};
  if (S.groundZones[id]) return {id, kind: 'ground_zone', item: S.groundZones[id]};
  return null;
}

function regionColor(id) {
  const index = Object.keys(S.regions).sort().indexOf(id);
  return COLORS[(index < 0 ? 0 : index) % COLORS.length];
}

function lineColor() { return '#ff45ef'; }
function zoneColor(id) {
  const index = Object.keys(S.groundZones).sort().indexOf(id);
  return COLORS[(index < 0 ? 0 : index) % COLORS.length];
}

function itemPoints(entry) {
  if (!entry) return [];
  if (entry.kind === 'region') return Array.isArray(entry.item.points) ? entry.item.points : [];
  if (entry.kind === 'line') return [entry.item.p1, entry.item.p2].filter(validPoint);
  if (entry.kind === 'ground_zone') return Array.isArray(entry.item.points_img) ? entry.item.points_img : [];
  return [];
}

function clonePoints(points) {
  return Array.isArray(points) ? points.map(p => Array.isArray(p) ? p.slice() : p) : [];
}

function imageMoveGeometry(entry) {
  const geo = {kind: entry.kind};
  if (entry.kind === 'region') {
    const pointsMode = entry.item.role === 'ground' && isGroundPoints(entry.item);
    geo.pointsMode = pointsMode;
    geo.points = pointsMode ? [] : clonePoints(entry.item.points);
    geo.calib_points = pointsMode ? clonePoints(entry.item.calib_points) : [];
    geo.check_points = pointsMode ? clonePoints(entry.item.check_points) : [];
    geo.extent = [...geo.points, ...geo.calib_points, ...geo.check_points].filter(validPoint);
  } else if (entry.kind === 'line') {
    geo.p1 = validPoint(entry.item.p1) ? entry.item.p1.slice() : null;
    geo.p2 = validPoint(entry.item.p2) ? entry.item.p2.slice() : null;
    geo.extent = [geo.p1, geo.p2].filter(validPoint);
  } else if (entry.kind === 'ground_zone') {
    geo.points = clonePoints(entry.item.points_img);
    geo.extent = geo.points.filter(validPoint);
  }
  return geo.extent.length ? geo : null;
}

function shiftPointList(points, dx, dy) {
  return (points || []).map(p => validPoint(p)
    ? [round4(Number(p[0]) + dx), round4(Number(p[1]) + dy), ...p.slice(2)] : p);
}

function applyImageMove(entry, geo, dx, dy) {
  if (!geo?.extent?.length) return false;
  dx = Math.max(-Math.min(...geo.extent.map(p => p[0])),
                Math.min(dx, 1 - Math.max(...geo.extent.map(p => p[0]))));
  dy = Math.max(-Math.min(...geo.extent.map(p => p[1])),
                Math.min(dy, 1 - Math.max(...geo.extent.map(p => p[1]))));
  if (Math.abs(dx) < 1e-9 && Math.abs(dy) < 1e-9) return false;
  if (entry.kind === 'region') {
    if (geo.pointsMode) {
      entry.item.calib_points = shiftPointList(geo.calib_points, dx, dy);
      if (geo.check_points.length) entry.item.check_points = shiftPointList(geo.check_points, dx, dy);
      updateCalibHull(entry.item);
    } else {
      entry.item.points = shiftPointList(geo.points, dx, dy);
    }
  } else if (entry.kind === 'line') {
    entry.item.p1 = geo.p1 ? [round4(geo.p1[0] + dx), round4(geo.p1[1] + dy)] : geo.p1;
    entry.item.p2 = geo.p2 ? [round4(geo.p2[0] + dx), round4(geo.p2[1] + dy)] : geo.p2;
  } else if (entry.kind === 'ground_zone') {
    entry.item.points_img = shiftPointList(geo.points, dx, dy);
  }
  return true;
}

function nudgeSelected(dx, dy) {
  const entry = findRegionItem();
  if (!entry) return false;
  const geo = imageMoveGeometry(entry);
  if (!geo || !applyImageMove(entry, geo, dx, dy)) return false;
  markDirty();
  renderRegionInspector();
  return true;
}

function nudgeControls(entry) {
  if (!imageMoveGeometry(entry)) return '';
  return `<div class="nudge-controls" aria-label="位置微调">
    <button class="btn tiny ghost" data-insp-action="nudge-up" title="上移">↑</button>
    <button class="btn tiny ghost" data-insp-action="nudge-left" title="左移">←</button>
    <button class="btn tiny ghost" data-insp-action="nudge-down" title="下移">↓</button>
    <button class="btn tiny ghost" data-insp-action="nudge-right" title="右移">→</button>
    <span>拖动区域内部整体平移，方向键微调；Shift 加速</span>
  </div>`;
}

function validPoint(p) {
  if (!Array.isArray(p) || p.length < 2) return false;
  return [p[0], p[1]].every(v =>
    v !== '' && v != null && Object.prototype.toString.call(v) !== '[object Array]' &&
    Number.isFinite(Number(v)));
}

function ruleEnabled(item, key) {
  const rule = item?.rules?.[key];
  return Boolean(rule && typeof rule === 'object' && rule.enabled !== false);
}

function newId(prefix) {
  const used = new Set([...Object.keys(S.regions), ...Object.keys(S.lines), ...Object.keys(S.groundZones)]);
  let i = 1;
  while (used.has(`${prefix}-${i}`)) i++;
  return `${prefix}-${i}`;
}

function ensureRegionEditor() {
  if (!Object.keys(S.regions).length && !Object.keys(S.lines).length && S.dirty) return;
  if (!S.config || !Object.keys(S.config).length) loadConfig();
  if (S.status?.power?.mode === 'standby') {
    S.editorImageReady = false;
    $('#editor-loading').textContent = '设备处于待机。先打开摄像头，才能加载区域编辑底图。';
    $('#editor-loading').classList.remove('hidden');
  } else {
    loadEditorImage();
  }
  if (S.showHeatmap || getPath(S.config, 'display.show_heatmap', false)) loadHeat();
}

function loadEditorImage(useFallback = false) {
  const img = $('#editor-image');
  $('#editor-loading').classList.remove('hidden');
  $('#editor-loading').textContent = '正在加载底图…';
  img.onload = () => {
    S.editorImageReady = true;
    $('#editor-canvas').classList.remove('hidden');
    syncEditorCanvasSize();
    $('#editor-loading').classList.add('hidden');
    renderRegionCanvas();
    renderHeatOverlay();
  };
  img.onerror = () => {
    // A failed image keeps its layout size, so the canvas can stay over the
    // previous frame. Hide it while the fallback URL is tried.
    $('#editor-canvas').classList.add('hidden');
    if (!useFallback) {
      setTimeout(() => loadEditorImage(true), 250);
      return;
    }
    S.editorImageReady = false;
    const standby = S.status?.power?.mode === 'standby';
    $('#editor-loading').textContent = standby
      ? '设备处于待机。先打开摄像头，才能加载区域编辑底图。'
      : '当前没有可用画面，正在等待摄像头画面…';
    $('#editor-loading').classList.remove('hidden');
  };
  const base = useFallback ? '/snapshot.jpg' : '/snapshot.jpg?raw=1';
  img.src = `${base}${base.includes('?') ? '&' : '?'}t=${Date.now()}`;
}

function syncEditorCanvasSize() {
  const canvas = $('#editor-canvas');
  const img = $('#editor-image');
  if (!canvas || !img) return false;
  const width = Math.round(img.getBoundingClientRect().width || img.clientWidth || img.naturalWidth || 0);
  const height = Math.round(img.getBoundingClientRect().height || img.clientHeight || img.naturalHeight || 0);
  if (width <= 0 || height <= 0) return false;
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  return true;
}

function renderRegionEditor() {
  if (!S.config || !Object.keys(S.config).length) return;
  const show = Boolean(getPath(S.config, 'display.show_heatmap', false));
  if (!$('#show-heatmap').matches(':focus')) $('#show-heatmap').checked = S.showHeatmap || show;
  S.showHeatmap = $('#show-heatmap').checked;
  $('#heat-learning').checked = Boolean(getPath(S.config, 'activity.learning', false));
  renderRegionList();
  renderRegionInspector();
  renderRegionCanvas();
  renderHeatOverlay();
  updateSaveButton();
  if (S.regionView) {
    $('#draw-hint').textContent = '摄像头画面有偏移，区域已按自动跟随后的位置显示；保存后以当前画面为准。';
  }
}

function renderRegionList() {
  const box = $('#region-list');
  if (!box) return;
  const focusedId = document.activeElement?.matches?.('[data-select-id]')
    ? document.activeElement.dataset.selectId : null;
  const parts = [];
  const group = (label, rows) => {
    if (!rows.length) return;
    parts.push(`<div class="region-group-label">${esc(label)}</div>`);
    parts.push(...rows);
  };
  group('区域', Object.entries(S.regions).map(([id, r]) => {
    const bindings = Object.keys(r.rules || {}).filter(k => ruleEnabled(r, k))
      .map(k => translateExact(FEATURE_NAMES[k] || k))
      .join(currentLanguage === 'en' ? ', ' : '、');
    return `<div class="region-row ${id === S.selectedId ? 'active' : ''}" tabindex="0" role="button" data-select-id="${esc(id)}">
      <span class="region-swatch" style="background:${regionColor(id)}"></span>
      <span><strong>${esc(r.name || id)}</strong><small>${esc(translateExact(ROLE_NAMES[r.role] || '未标注'))}${bindings ? ` · ${esc(bindings)}` : ''}</small></span>
      <span class="row-kind">${r.role === 'ground' ? '标定' : '区域'}</span></div>`;
  }));
  group('进出线', Object.entries(S.lines).map(([id, l]) => `
    <div class="region-row ${id === S.selectedId ? 'active' : ''}" tabindex="0" role="button" data-select-id="${esc(id)}">
      <span class="region-swatch" style="background:${lineColor()}"></span>
      <span><strong>${esc(l.name || id)}</strong><small>${ruleEnabled(l, 'count') ? '人数统计' : '未绑定功能'}</small></span>
      <span class="row-kind">线</span></div>`));
  group('地面区域', Object.entries(S.groundZones).map(([id, z]) => `
    <div class="region-row ${id === S.selectedId ? 'active' : ''}" tabindex="0" role="button" data-select-id="${esc(id)}">
      <span class="region-swatch" style="background:${zoneColor(id)}"></span>
      <span><strong>${esc(z.name || id)}</strong><small>${z.points_m ? '米制坐标' : '画面坐标'}${z.enabled === false ? ' · 已停用' : ''}</small></span>
      <span class="row-kind">地面</span></div>`));
  box.innerHTML = parts.length ? parts.join('') : '<p class="muted">还没有区域或线，点击上方工具开始绘制。</p>';
  $('#region-count').textContent = translatePhrase(`${Object.keys(S.regions).length + Object.keys(S.lines).length + Object.keys(S.groundZones).length} 项`);
  $$('[data-select-id]', box).forEach(row => {
    row.addEventListener('click', () => selectRegion(row.dataset.selectId));
    row.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        selectRegion(row.dataset.selectId);
      }
    });
  });
  if (focusedId) $(`[data-select-id="${CSS.escape(focusedId)}"]`, box)?.focus();
}

function selectRegion(id) {
  S.selectedId = id;
  S.draft = [];
  S.drawMode = null;
  renderRegionList();
  renderRegionInspector();
  renderRegionCanvas();
  updateDrawButtons();
}

function isGroundPoints(region) {
  return Boolean(region && (region.type === 'points' || (Array.isArray(region.calib_points) && region.calib_points.length)));
}

function renderRegionInspector() {
  const box = $('#region-inspector');
  const entry = findRegionItem();
  if (!entry) {
    box.innerHTML = '<p class="muted">选择一个区域、线或地面区域进行编辑。</p>';
    return;
  }
  const {id, kind, item} = entry;
  if (kind === 'ground_zone') {
    const groups = Object.keys(getPath(S.config, 'features.locate.groups', {person: [], vehicle: [], pet: []}));
    const meters = Array.isArray(item.points_m);
    const points = (meters ? item.points_m : item.points_img || []).filter(validPoint);
    const coordRows = points.map((p, i) => `<tr>
      <td>${i + 1}</td>
      <td>${meters ? `<input type="number" step="0.05" data-zone-xy="${i}.0" value="${Number(p[0])}">` : fmt(Number(p[0]), 3)}</td>
      <td>${meters ? `<input type="number" step="0.05" data-zone-xy="${i}.1" value="${Number(p[1])}">` : fmt(Number(p[1]), 3)}</td>
      <td><button class="btn tiny ghost" data-insp-action="remove-zone-point" data-index="${i}">${currentLanguage === 'en' ? 'Remove' : '删'}</button></td></tr>`).join('');
    box.innerHTML = `<div class="inspector-grid">
      <label class="field"><span>名称</span><input data-insp-field="name" value="${esc(item.name || id)}"></label>
      <div class="field-grid">
        <label class="field"><span>坐标来源</span><select data-zone-source>
          <option value="image" ${meters ? '' : 'selected'}>画面坐标（随地面标定换算）</option>
          <option value="meters" ${meters ? 'selected' : ''}>米制坐标（鸟瞰图）</option>
        </select></label>
        <label class="field"><span>顶点</span><input value="${points.length} ${currentLanguage === 'en' ? 'vertices' : '个'}" disabled></label>
      </div>
      ${nudgeControls(entry)}
      <div class="toggle-list">
        <label class="toggle-row"><span><strong>启用此地面区域</strong><small>不影响地面定位总开关</small></span><input type="checkbox" data-zone-field="enabled" ${item.enabled !== false ? 'checked' : ''}></label>
        <label class="toggle-row"><span><strong>进入时告警</strong><small>关闭时进入只记一条信息事件</small></span><input type="checkbox" data-zone-field="alarm_enter" ${item.alarm_enter ? 'checked' : ''}></label>
      </div>
      <label class="field"><span>停留超时告警（秒，0 = 不报）</span><input data-zone-field="dwell_sec" type="number" min="0" step="0.1" value="${Number(item.dwell_sec || 0)}"></label>
      <h3>目标分组（都不勾 = 全部）</h3>
      <div class="binding-list">${groups.map(g => `<label class="binding-row"><span>${esc({person:'人员',vehicle:'车辆',pet:'宠物'}[g] || g)}</span><input type="checkbox" data-zone-group="${esc(g)}" ${(item.groups || []).includes(g) ? 'checked' : ''}></label>`).join('')}</div>
      <h3>顶点坐标${meters ? '（米）' : '（画面 0～1，只读）'}</h3>
      <div class="table-scroll" style="max-height:220px"><table class="calib-table"><thead><tr><th>点</th><th>${meters ? 'X(m)' : 'U'}</th><th>${meters ? 'Y(m)' : 'V'}</th><th></th></tr></thead><tbody>${coordRows || '<tr><td colspan="4" class="muted">还没有顶点</td></tr>'}</tbody></table></div>
      ${meters ? '' : '<p class="muted">在左侧画面拖动顶点调整形状；需要直接输入米制坐标时切换坐标来源。</p>'}
      <button class="btn danger" data-insp-action="delete">删除这个地面区域</button>
    </div>`;
    return;
  }
  if (kind === 'line') {
    const bound = ruleEnabled(item, 'count');
    box.innerHTML = `<div class="inspector-grid">
      <label class="field"><span>名称</span><input data-insp-field="name" value="${esc(item.name || id)}"></label>
      <div class="field-grid">
        <button class="btn secondary" data-insp-action="flip-line">交换进出方向</button>
        <button class="btn ghost" data-insp-action="center-line">恢复水平线</button>
      </div>
      ${nudgeControls(entry)}
      <h3>功能绑定</h3>
      <div class="binding-list">${LINE_BINDINGS.map(([key, name]) => `<label class="binding-row"><span>${esc(name)}</span><input type="checkbox" data-bind="${key}" ${bound ? 'checked' : ''}></label>`).join('')}</div>
      <p class="muted">线按 p1 → p2 定向；只在“人数统计”启用且绑定后参与计数。</p>
      <button class="btn danger" data-insp-action="delete">删除这条进出线</button>
    </div>`;
    return;
  }

  const role = item.role || '';
  const ground = role === 'ground';
  const pointsMode = ground && isGroundPoints(item);
  const allowedBindings = REGION_BINDINGS.filter(([key]) => regionBindingAllowed(key, role));
  const bindings = allowedBindings.map(([key, name]) => {
    const active = ruleEnabled(item, key);
    let extra = '';
    if (active && ['intrusion','fence','dwell'].includes(key)) {
      const defaultField = key === 'dwell' ? 'dwell_sec' : 'sustain_sec';
      const value = item.rules[key]?.[defaultField] ?? '';
      const speaker = item.rules[key]?.actions?.speaker?.enabled;
      extra = `<div class="binding-extra">
        <label class="field"><span>${key === 'dwell' ? '此处滞留时间（秒）' : '此处持续确认（秒）'}</span><input type="number" min="0" step="0.5" data-override="${key}.${defaultField}" value="${esc(value)}" placeholder="使用全局值"></label>
        <label class="field"><span>此处声音</span><select data-speaker="${key}"><option value="default" ${speaker === undefined ? 'selected' : ''}>跟随全局</option><option value="true" ${speaker === true ? 'selected' : ''}>开启</option><option value="false" ${speaker === false ? 'selected' : ''}>关闭</option></select></label>
      </div>`;
    }
    return `<div><label class="binding-row"><span>${esc(name)}</span><input type="checkbox" data-bind="${key}" ${active ? 'checked' : ''}></label>${extra}</div>`;
  }).join('');

  let groundHtml = '';
  if (ground) {
    groundHtml = `<h3>地面标定</h3>
      <div class="field-grid">
        <label class="field"><span>标定方式</span><select data-ground-mode><option value="rect" ${pointsMode ? '' : 'selected'}>矩形 4 点</option><option value="points" ${pointsMode ? 'selected' : ''}>多点对应（≥4 点）</option></select></label>
        ${pointsMode ? '<label class="field"><span>轮廓</span><input value="由标定点凸包生成" disabled></label>' :
          `<label class="field"><span>实际宽度（米）</span><input data-ground-size="0" type="number" min="0.01" step="0.1" value="${Number(item.real_size_m?.[0] || 4)}"></label>
           <label class="field"><span>实际纵深（米）</span><input data-ground-size="1" type="number" min="0.01" step="0.1" value="${Number(item.real_size_m?.[1] || 8)}"></label>`}
      </div>`;
    if (pointsMode) {
      const rows = (item.calib_points || []).map((c, i) => `<tr>
        <td>${i + 1}</td><td>${fmt(Number(c[0]), 3)}</td><td>${fmt(Number(c[1]), 3)}</td>
        <td><input type="number" step="0.1" data-calib-xy="${i}.0" value="${Number(c[2] || 0)}"></td>
        <td><input type="number" step="0.1" data-calib-xy="${i}.1" value="${Number(c[3] || 0)}"></td>
        <td><button class="btn tiny ghost" data-insp-action="remove-calib" data-index="${i}">删</button></td></tr>`).join('');
      const checks = (item.check_points || []).map((c, i) => `<tr>
        <td>C${i + 1}</td><td>${fmt(Number(c[0]), 3)}</td><td>${fmt(Number(c[1]), 3)}</td>
        <td><input type="number" step="0.1" data-check-xy="${i}.0" value="${Number(c[2] || 0)}"></td>
        <td><input type="number" step="0.1" data-check-xy="${i}.1" value="${Number(c[3] || 0)}"></td>
        <td><button class="btn tiny ghost" data-insp-action="remove-check" data-index="${i}">删</button></td></tr>`).join('');
      groundHtml += `<div class="table-scroll" style="max-height:260px"><table class="calib-table"><thead><tr><th>点</th><th>U</th><th>V</th><th>X(m)</th><th>Y(m)</th><th></th></tr></thead><tbody>${rows || '<tr><td colspan="6" class="muted">还没有标定点</td></tr>'}${checks}</tbody></table></div>
        <div class="mini-actions"><button class="btn tiny secondary" data-insp-action="add-calib">在画面上添加标定点</button><button class="btn tiny ghost" data-insp-action="add-check">添加检查点</button></div>`;
    }
    groundHtml += `<p class="muted">矩形模式点四个地面角；多点模式可把额外测量点换算到同一坐标系内。</p>`;
  }

  box.innerHTML = `<div class="inspector-grid">
    <label class="field"><span>名称</span><input data-insp-field="name" value="${esc(item.name || id)}"></label>
    <div class="field-grid">
      <label class="field"><span>用途</span><select data-insp-role>
        <option value="" ${role === '' ? 'selected' : ''}>未标注</option>
        <option value="private" ${role === 'private' ? 'selected' : ''}>自家区域</option>
        <option value="public" ${role === 'public' ? 'selected' : ''}>公共区域</option>
        <option value="ignore" ${role === 'ignore' ? 'selected' : ''}>忽略区</option>
        <option value="ground" ${role === 'ground' ? 'selected' : ''}>地面标定</option>
      </select></label>
      <label class="field"><span>顶点</span><input value="${Array.isArray(item.points) ? item.points.length : 0} ${currentLanguage === 'en' ? 'vertices' : '个'}" disabled></label>
    </div>
    ${nudgeControls(entry)}
    ${groundHtml}
    <h3>功能绑定</h3>
    <div class="binding-list">${bindings || '<p class="muted">当前用途没有可绑定的功能。</p>'}</div>
    <p class="muted">最多可创建 12 个区域（含地面标定和忽略区）。同一功能可以绑定多个区域；定位只能使用一个地面标定区域。修改后点击顶部“保存区域”。</p>
    <button class="btn danger" data-insp-action="delete">删除当前区域</button>
  </div>`;
}

function updateSaveButton() {
  const btn = $('#regions-save');
  if (btn) {
    btn.disabled = !S.dirty;
    btn.textContent = S.dirty ? '保存区域 *' : '保存区域';
  }
}

function markDirty() {
  S.dirty = true;
  updateSaveButton();
  renderRegionCanvas();
}

function renderRegionCanvas() {
  const canvas = $('#editor-canvas');
  if (!canvas || !S.editorImageReady || !syncEditorCanvasSize()) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  if (S.showHeatmap && S.heat) drawHeatGrid(ctx, S.heat, w, h, 0.46);
  const scale = Math.max(1, w / 1280);

  for (const [id, r] of Object.entries(S.regions)) {
    const pts = (r.points || []).filter(validPoint);
    if (!pts.length) continue;
    const selected = id === S.selectedId;
    const color = regionColor(id);
    ctx.beginPath();
    ctx.moveTo(pts[0][0] * w, pts[0][1] * h);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0] * w, pts[i][1] * h);
    if (pts.length >= 3) ctx.closePath();
    ctx.fillStyle = hexToRgba(color, selected ? 0.2 : 0.09);
    if (pts.length >= 3) ctx.fill();
    ctx.strokeStyle = color;
    ctx.lineWidth = (selected ? 3.2 : 2.0) * scale;
    ctx.stroke();
    if (selected && !(r.role === 'ground' && isGroundPoints(r))) drawHandles(ctx, pts, w, h, color, scale);
    drawLabel(ctx, r.name || id, centroid(pts), w, h, color, scale);
  }
  for (const [id, z] of Object.entries(S.groundZones)) {
    if (!Array.isArray(z.points_img) || z.points_img.length < 3) continue;
    const pts = z.points_img.filter(validPoint);
    if (pts.length < 3) continue;
    const selected = id === S.selectedId;
    const color = zoneColor(id);
    ctx.beginPath();
    ctx.moveTo(pts[0][0] * w, pts[0][1] * h);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0] * w, pts[i][1] * h);
    ctx.closePath();
    ctx.fillStyle = hexToRgba(color, selected ? 0.22 : 0.09);
    ctx.fill();
    ctx.strokeStyle = color;
    ctx.lineWidth = (selected ? 3 : 1.7) * scale;
    ctx.stroke();
    if (selected) drawHandles(ctx, pts, w, h, color, scale);
    drawLabel(ctx, z.name || id, centroid(pts), w, h, color, scale);
  }
  for (const [id, line] of Object.entries(S.lines)) {
    if (!validPoint(line.p1) || !validPoint(line.p2)) continue;
    const selected = id === S.selectedId;
    const p1 = line.p1, p2 = line.p2;
    ctx.strokeStyle = lineColor();
    ctx.lineWidth = (selected ? 4 : 2.4) * scale;
    ctx.beginPath();
    ctx.moveTo(p1[0] * w, p1[1] * h);
    ctx.lineTo(p2[0] * w, p2[1] * h);
    ctx.stroke();
    if (selected) drawHandles(ctx, [p1, p2], w, h, lineColor(), scale);
    const dx = p2[0] - p1[0], dy = p2[1] - p1[1];
    const len = Math.hypot(dx, dy) || 1;
    const nx = -dy / len, ny = dx / len;
    const mx = (p1[0] + p2[0]) / 2 * w, my = (p1[1] + p2[1]) / 2 * h;
    ctx.beginPath();
    ctx.moveTo(mx, my);
    ctx.lineTo(mx + nx * 18 * scale, my + ny * 18 * scale);
    ctx.lineTo(mx + (nx - dx / len * .4) * 12 * scale, my + (ny - dy / len * .4) * 12 * scale);
    ctx.stroke();
    drawLabel(ctx, `${line.name || id} · IN`, [mx / w, my / h], w, h, lineColor(), scale);
  }
  const entry = findRegionItem();
  const drafting = S.drawMode && S.draft.length;
  if (drafting) {
    const pts = S.draft;
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 2 * scale;
    ctx.setLineDash([8 * scale, 5 * scale]);
    ctx.beginPath();
    ctx.moveTo(pts[0][0] * w, pts[0][1] * h);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0] * w, pts[i][1] * h);
    ctx.stroke();
    ctx.setLineDash([]);
    drawHandles(ctx, pts, w, h, '#ffffff', scale);
  }
  if (entry?.kind === 'region' && entry.item.role === 'ground' && isGroundPoints(entry.item)) {
    for (const c of entry.item.calib_points || []) {
      if (!validPoint(c)) continue;
      ctx.beginPath();
      ctx.arc(c[0] * w, c[1] * h, 5 * scale, 0, Math.PI * 2);
      ctx.fillStyle = '#5ea8ff'; ctx.fill();
      ctx.fillStyle = '#fff'; ctx.font = `${11 * scale}px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif`; ctx.fillText(`X${fmt(Number(c[2]), 1)} Y${fmt(Number(c[3]), 1)}`, c[0] * w + 8 * scale, c[1] * h - 8 * scale);
    }
    for (const c of entry.item.check_points || []) {
      if (!validPoint(c)) continue;
      ctx.strokeStyle = '#f0b84b'; ctx.lineWidth = 2 * scale;
      ctx.beginPath(); ctx.arc(c[0] * w, c[1] * h, 5 * scale, 0, Math.PI * 2); ctx.stroke();
    }
  }
}

function drawHeatGrid(ctx, heat, w, h, alpha) {
  const cols = Number(heat.cols || 64), rows = Number(heat.rows || 36);
  const people = Array.isArray(heat.person) ? heat.person : [];
  const vehicles = Array.isArray(heat.vehicle) ? heat.vehicle : [];
  let max = 0;
  for (let i = 0; i < cols * rows; i++) max = Math.max(max, Number(people[i] || 0) + Number(vehicles[i] || 0));
  if (max <= 0) return;
  const cw = w / cols, ch = h / rows;
  for (let y = 0; y < rows; y++) {
    for (let x = 0; x < cols; x++) {
      const v = Number(people[y * cols + x] || 0) + Number(vehicles[y * cols + x] || 0);
      if (!v) continue;
      const a = Math.pow(v / max, .7) * alpha;
      const hue = 205 - 205 * Math.pow(v / max, .9);
      ctx.fillStyle = `hsla(${hue},100%,56%,${a})`;
      ctx.fillRect(x * cw, y * ch, cw + .5, ch + .5);
    }
  }
}

function drawHandles(ctx, pts, w, h, color, scale) {
  for (const p of pts) {
    if (!validPoint(p)) continue;
    ctx.beginPath();
    ctx.arc(p[0] * w, p[1] * h, 6 * scale, 0, Math.PI * 2);
    ctx.fillStyle = '#fff';
    ctx.fill();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2 * scale;
    ctx.stroke();
  }
}

function drawLabel(ctx, text, p, w, h, color, scale) {
  if (!p) return;
  const x = p[0] * w, y = p[1] * h;
  ctx.font = `600 ${11 * scale}px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif`;
  const width = ctx.measureText(text).width + 12 * scale;
  ctx.fillStyle = 'rgba(0,0,0,.62)';
  ctx.fillRect(x - width / 2, y - 8 * scale, width, 18 * scale);
  ctx.fillStyle = color;
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.fillText(text, x, y + scale);
  ctx.textAlign = 'start'; ctx.textBaseline = 'alphabetic';
}

function centroid(pts) {
  if (!pts.length) return null;
  return [pts.reduce((a, p) => a + p[0], 0) / pts.length, pts.reduce((a, p) => a + p[1], 0) / pts.length];
}

function hexToRgba(hex, alpha) {
  const h = hex.replace('#', '');
  const n = parseInt(h.length === 3 ? h.split('').map(c => c + c).join('') : h, 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${alpha})`;
}

function canvasPoint(evt) {
  const image = $('#editor-image');
  const rect = image.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return [0, 0];
  return [
    Math.max(0, Math.min(1, (evt.clientX - rect.left) / rect.width)),
    Math.max(0, Math.min(1, (evt.clientY - rect.top) / rect.height))
  ];
}

function canvasEventPoint(evt) {
  const image = $('#editor-image');
  const rect = image.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return null;
  return [
    (evt.clientX - rect.left) / rect.width,
    (evt.clientY - rect.top) / rect.height
  ];
}

function hitHandle(p) {
  const entry = findRegionItem();
  if (!entry) return null;
  if (entry.kind === 'region' && entry.item.role === 'ground' && isGroundPoints(entry.item)) return null;
  const pts = itemPoints(entry);
  const image = $('#editor-image');
  const threshold = 12 / Math.max(1, image.getBoundingClientRect().width);
  for (let i = 0; i < pts.length; i++) {
    if (pts[i] && Math.hypot(p[0] - pts[i][0], p[1] - pts[i][1]) <= threshold) return {index: i};
  }
  return null;
}

function hitItem(p) {
  for (const [id, z] of Object.entries(S.groundZones).reverse()) {
    const pts = Array.isArray(z.points_img) ? z.points_img.filter(validPoint) : [];
    if (pts.length >= 3 && pointInPolygon(pts, p)) return id;
  }
  for (const [id, r] of Object.entries(S.regions).reverse()) {
    const pts = Array.isArray(r.points) ? r.points.filter(validPoint) : [];
    if (pts.length >= 3 && pointInPolygon(pts, p)) return id;
  }
  let best = null;
  for (const [id, l] of Object.entries(S.lines)) {
    const d = distToSegment(p, l.p1, l.p2);
    if (d < .015 && (!best || d < best.d)) best = {id, d};
  }
  return best?.id || null;
}

function pointInPolygon(pts, p) {
  let inside = false;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
    if (((yi > p[1]) !== (yj > p[1])) && (p[0] < (xj - xi) * (p[1] - yi) / ((yj - yi) || 1e-9) + xi)) inside = !inside;
  }
  return inside;
}

function distToSegment(p, a, b) {
  if (!validPoint(a) || !validPoint(b)) return Infinity;
  const vx = b[0] - a[0], vy = b[1] - a[1];
  const wx = p[0] - a[0], wy = p[1] - a[1];
  const len2 = vx * vx + vy * vy;
  const t = len2 ? Math.max(0, Math.min(1, (wx * vx + wy * vy) / len2)) : 0;
  return Math.hypot(p[0] - (a[0] + t * vx), p[1] - (a[1] + t * vy));
}

function bindEditor() {
  const canvas = $('#editor-canvas');
  canvas.addEventListener('pointerdown', e => {
    const raw = canvasEventPoint(e);
    if (!raw) return;
    const p = canvasPoint(e);
    if (S.drawMode) {
      // Ignore clicks outside the image when the stage is letterboxed.
      if (raw[0] < 0 || raw[0] > 1 || raw[1] < 0 || raw[1] > 1) return;
      handleDrawClick(p);
      return;
    }
    const handle = hitHandle(p);
    if (handle) {
      S.drag = {kind: findRegionItem().kind, id: S.selectedId, index: handle.index};
      canvas.setPointerCapture(e.pointerId);
      return;
    }
    const id = hitItem(p);
    selectRegion(id);
    const entry = findRegionItem(id);
    const geometry = entry ? imageMoveGeometry(entry) : null;
    if (geometry) {
      S.drag = {kind: entry.kind, id, index: null, start: p, geometry};
      canvas.setPointerCapture(e.pointerId);
    }
  });
  canvas.addEventListener('pointermove', e => {
    if (!S.drag) return;
    const p = canvasPoint(e);
    const entry = findRegionItem(S.drag.id);
    if (!entry) return;
    if (S.drag.index === null) {
      if (!applyImageMove(entry, S.drag.geometry,
                          p[0] - S.drag.start[0], p[1] - S.drag.start[1])) return;
      S.dirty = true;
      updateSaveButton();
      renderRegionCanvas();
      return;
    }
    if (entry.kind === 'region') {
      entry.item.points[S.drag.index] = [round4(p[0]), round4(p[1])];
      if (entry.item.role === 'ground' && isGroundPoints(entry.item)) updateCalibHull(entry.item);
    } else if (entry.kind === 'line') {
      if (S.drag.index === 0) entry.item.p1 = [round4(p[0]), round4(p[1])];
      else entry.item.p2 = [round4(p[0]), round4(p[1])];
    } else if (entry.kind === 'ground_zone' && Array.isArray(entry.item.points_img)) {
      entry.item.points_img[S.drag.index] = [round4(p[0]), round4(p[1])];
    }
    S.dirty = true;
    updateSaveButton();
    renderRegionCanvas();
  });
  const endDrag = () => { S.drag = null; };
  canvas.addEventListener('pointerup', endDrag);
  canvas.addEventListener('pointercancel', endDrag);
  canvas.addEventListener('dblclick', e => {
    if (S.drawMode && ['region','gzone','ground'].includes(S.drawMode)) {
      e.preventDefault();
      finishDrawing();
    }
  });

  $('#draw-region').addEventListener('click', () => startDrawing('region'));
  $('#draw-line').addEventListener('click', () => startDrawing('line'));
  $('#draw-ground').addEventListener('click', () => startDrawing('ground'));
  $('#draw-gzone').addEventListener('click', () => startDrawing('gzone'));
  $('#draw-finish').addEventListener('click', finishDrawing);
  $('#draw-undo').addEventListener('click', () => {
    S.draft.pop();
    renderRegionCanvas();
    updateDrawButtons();
  });
  $('#draw-cancel').addEventListener('click', cancelDrawing);
  $('#regions-save').addEventListener('click', saveRegions);
  $('#show-heatmap').addEventListener('change', async e => {
    S.showHeatmap = e.target.checked;
    if (S.showHeatmap) {
      await loadHeat();
      saveConfigPatch({display: {show_heatmap: true}}, {message: '热图显示已开启', keepRegions: true}).catch(err => toast(err.message, 'error'));
    } else {
      saveConfigPatch({display: {show_heatmap: false}}, {message: '热图显示已关闭', keepRegions: true}).catch(err => toast(err.message, 'error'));
    }
    renderRegionCanvas(); renderHeatOverlay();
  });
  $('#heat-learning').addEventListener('change', async e => {
    try { await saveConfigPatch({activity: {learning: e.target.checked}}, {message: `热图学习已${e.target.checked ? '开启' : '关闭'}`, keepRegions: true}); }
    catch (err) { e.target.checked = !e.target.checked; toast(err.message, 'error'); }
  });
  $('#heat-suggest').addEventListener('click', suggestFromHeat);
  $('#heat-clear').addEventListener('click', async () => {
    if (!confirm('清空活动热图？已统计的人/车活动会全部清零。')) return;
    try { await api('/heatmap', {method: 'DELETE'}); toast('热图已清空', 'ok'); loadHeat(true); }
    catch (err) { toast(`清空失败: ${err.message}`, 'error'); }
  });

  const inspector = $('#region-inspector');
  inspector.addEventListener('input', e => {
    const entry = findRegionItem();
    if (!entry) return;
    if (e.target.matches('[data-insp-field="name"]')) {
      entry.item.name = e.target.value.trim() || entry.id;
      markDirty();
      renderRegionList();
    }
  });
  inspector.addEventListener('change', e => {
    const entry = findRegionItem();
    if (!entry) return;
    const target = e.target;
    if (target.matches('[data-insp-role]')) { changeRegionRole(entry, target.value); return; }
    if (target.matches('[data-ground-mode]')) { changeGroundMode(entry, target.value); return; }
    if (target.matches('[data-ground-size]')) {
      const value = Number(target.value);
      if (!Number.isFinite(value) || value <= 0) {
        toast('实际宽度和纵深必须大于 0', 'error'); renderRegionInspector(); return;
      }
      entry.item.real_size_m = entry.item.real_size_m || [4, 8];
      entry.item.real_size_m[Number(target.dataset.groundSize)] = value;
      markDirty();
      return;
    }
    if (target.matches('[data-bind]')) {
      const key = target.dataset.bind;
      entry.item.rules = entry.item.rules || {};
      if (target.checked) {
        entry.item.rules[key] = entry.item.rules[key] || {};
        if (entry.item.rules[key].enabled === false) delete entry.item.rules[key].enabled;
        if (key === 'locate') {
          for (const [id, r] of Object.entries(S.regions)) {
            if (id !== entry.id && r.rules) delete r.rules.locate;
          }
        }
      } else delete entry.item.rules[key];
      markDirty(); renderRegionList(); renderRegionInspector(); return;
    }
    if (target.matches('[data-override]')) {
      const [key, field] = target.dataset.override.split('.');
      const params = entry.item.rules?.[key] || {};
      const value = Number(target.value);
      if (target.value === '') delete params[field];
      else if (Number.isFinite(value) && value >= 0) params[field] = value;
      else { toast('请输入有效的非负数字', 'error'); renderRegionInspector(); return; }
      entry.item.rules[key] = params; markDirty(); return;
    }
    if (target.matches('[data-speaker]')) {
      const key = target.dataset.speaker;
      const params = entry.item.rules?.[key] || {};
      params.actions = params.actions || {}; params.actions.speaker = params.actions.speaker || {};
      if (target.value === 'default') delete params.actions.speaker.enabled;
      else params.actions.speaker.enabled = target.value === 'true';
      entry.item.rules[key] = params; markDirty(); return;
    }
    if (target.matches('[data-zone-source]')) {
      if (Array.isArray(entry.item.points_m) || Array.isArray(entry.item.points_img)) {
        changeGroundZoneSource(entry, target.value);
      }
      return;
    }
    if (target.matches('[data-zone-field]')) {
      if (target.dataset.zoneField === 'dwell_sec') {
        const value = Number(target.value);
        if (!Number.isFinite(value) || value < 0) {
          toast('停留时间必须是非负数字', 'error'); renderRegionInspector(); return;
        }
        entry.item.dwell_sec = value;
      } else {
        entry.item[target.dataset.zoneField] = target.type === 'checkbox' ? target.checked : target.value;
      }
      markDirty(); renderRegionList(); return;
    }
    if (target.matches('[data-zone-group]')) {
      const set = new Set(entry.item.groups || []);
      target.checked ? set.add(target.dataset.zoneGroup) : set.delete(target.dataset.zoneGroup);
      entry.item.groups = [...set]; markDirty(); renderRegionList(); return;
    }
    if (target.matches('[data-calib-xy]')) {
      const [index, axis] = target.dataset.calibXy.split('.').map(Number);
      const points = entry.item.calib_points || [];
      const value = Number(target.value);
      if (!points[index]) return;
      if (!Number.isFinite(value)) { toast('请输入有效的数字', 'error'); return; }
      points[index][2 + axis] = value;
      markDirty();
      return;
    }
    if (target.matches('[data-zone-xy]')) {
      const [index, axis] = target.dataset.zoneXy.split('.').map(Number);
      const points = entry.item.points_m || [];
      const value = Number(target.value);
      if (!points[index]) return;
      if (!Number.isFinite(value)) { toast('请输入有效的数字', 'error'); return; }
      points[index][axis] = value;
      markDirty();
      return;
    }
    if (target.matches('[data-check-xy]')) {
      const [index, axis] = target.dataset.checkXy.split('.').map(Number);
      const points = entry.item.check_points || [];
      const value = Number(target.value);
      if (!points[index]) return;
      if (!Number.isFinite(value)) { toast('请输入有效的数字', 'error'); return; }
      points[index][2 + axis] = value;
      markDirty();
      return;
    }
  });
  inspector.addEventListener('click', e => {
    const btn = e.target.closest('[data-insp-action]');
    if (!btn) return;
    const entry = findRegionItem();
    if (!entry) return;
    const action = btn.dataset.inspAction;
    if (action.startsWith('nudge-')) {
      const step = .005;
      const delta = {up: [0, -step], down: [0, step], left: [-step, 0], right: [step, 0]}[action.slice(6)];
      if (delta) nudgeSelected(delta[0], delta[1]);
    } else if (action === 'delete') deleteSelectedRegion();
    else if (action === 'flip-line') {
      [entry.item.p1, entry.item.p2] = [entry.item.p2, entry.item.p1]; markDirty(); renderRegionCanvas();
    } else if (action === 'center-line') {
      const cy = (entry.item.p1[1] + entry.item.p2[1]) / 2;
      entry.item.p1[1] = round4(cy); entry.item.p2[1] = round4(cy); markDirty(); renderRegionCanvas();
    } else if (action === 'add-calib') startCalibrationPoint(false);
    else if (action === 'add-check') startCalibrationPoint(true);
    else if (action === 'remove-calib') {
      entry.item.calib_points?.splice(Number(btn.dataset.index), 1); updateCalibHull(entry.item); markDirty(); renderRegionInspector(); renderRegionCanvas();
    } else if (action === 'remove-check') {
      entry.item.check_points?.splice(Number(btn.dataset.index), 1); markDirty(); renderRegionInspector(); renderRegionCanvas();
    } else if (action === 'remove-zone-point') {
      removeGroundZonePoint(entry, Number(btn.dataset.index));
    }
  });
}

function startDrawing(mode) {
  if (['region', 'ground'].includes(mode) && Object.keys(S.regions).length >= MAX_REGIONS) {
    toast(`区域最多只能有 ${MAX_REGIONS} 个；请先删除不用的区域`, 'error');
    return;
  }
  S.drawMode = mode;
  S.draft = [];
  S.selectedId = null;
  $('#draw-hint').textContent = ({
    region: '画区域：在画面上依次点击顶点，至少 3 点后点“完成”。',
    line: '画进出线：点击起点和终点，箭头指向 IN 一侧。',
    ground: '画地面标定：依次点击地面已知矩形的四个角。',
    gzone: '画地面区域：在画面上点击顶点，至少 3 点后点“完成”。'
  })[mode];
  renderRegionInspector(); renderRegionCanvas(); updateDrawButtons();
}

function handleDrawClick(p) {
  if (S.drawMode === 'region' || S.drawMode === 'ground' || S.drawMode === 'gzone') {
    S.draft.push([round4(p[0]), round4(p[1])]);
    if (S.drawMode === 'ground' && S.draft.length === 4) finishDrawing();
    else renderRegionCanvas();
  } else if (S.drawMode === 'line') {
    S.draft.push([round4(p[0]), round4(p[1])]);
    if (S.draft.length === 2) finishDrawing(); else renderRegionCanvas();
  } else if (S.drawMode === 'calib' || S.drawMode === 'check') {
    const entry = findRegionItem();
    if (!entry?.item) { cancelDrawing(); return; }
    const key = S.drawMode === 'check' ? 'check_points' : 'calib_points';
    entry.item[key] = entry.item[key] || [];
    entry.item[key].push([round4(p[0]), round4(p[1]), 0, 0]);
    if (key === 'calib_points') updateCalibHull(entry.item);
    markDirty(); renderRegionInspector(); renderRegionCanvas(); cancelDrawing(false);
  }
  updateDrawButtons();
}

function finishDrawing() {
  if (!S.drawMode || ['calib','check'].includes(S.drawMode)) return;
  if (S.drawMode === 'line' && S.draft.length !== 2) { toast('请点击线的起点和终点', 'error'); return; }
  if (['region','gzone','ground'].includes(S.drawMode) && S.draft.length < 3) { toast('至少需要 3 个点', 'error'); return; }
  if (S.drawMode === 'ground' && S.draft.length !== 4) { toast('地面标定必须正好 4 个点', 'error'); return; }
  if (['region','ground'].includes(S.drawMode) && Object.keys(S.regions).length >= MAX_REGIONS) {
    toast(`区域最多只能有 ${MAX_REGIONS} 个；请先删除不用的区域`, 'error');
    return;
  }
  if (['region','gzone'].includes(S.drawMode) && selfIntersects(S.draft)) { toast('边自相交，请按顺时针或逆时针依次点击', 'error'); return; }
  const points = S.draft.map(p => [round4(p[0]), round4(p[1])]);
  let id;
  if (S.drawMode === 'line') {
    id = newId('line');
    S.lines[id] = {name: `进出线 ${Object.keys(S.lines).length + 1}`, p1: points[0], p2: points[1], rules: {}};
  } else if (S.drawMode === 'gzone') {
    id = newId('gz');
    S.groundZones[id] = {name: `地面区域 ${Object.keys(S.groundZones).length + 1}`, points_img: points,
      enabled: true, alarm_enter: false, dwell_sec: 0, groups: []};
  } else if (S.drawMode === 'ground') {
    id = newId('ground');
    S.regions[id] = {type: 'quad', name: `地面标定 ${Object.values(S.regions).filter(r => r.role === 'ground').length + 1}`,
      role: 'ground', points: orderGroundQuad(points), real_size_m: [4, 8], rules: {}};
  } else {
    id = newId('region');
    S.regions[id] = {type: 'polygon', name: `区域 ${Object.keys(S.regions).length + 1}`,
      role: 'private', points, rules: {}};
  }
  S.selectedId = id;
  S.drawMode = null; S.draft = [];
  S.dirty = true;
  $('#draw-hint').textContent = '已创建，可在右侧修改属性并绑定功能。';
  renderRegionList(); renderRegionInspector(); renderRegionCanvas(); updateDrawButtons(); updateSaveButton();
}

function cancelDrawing(clearHint = true) {
  S.drawMode = null; S.draft = [];
  if (clearHint) $('#draw-hint').textContent = '已取消绘制。';
  updateDrawButtons(); renderRegionCanvas();
}

function startCalibrationPoint(check) {
  if (!S.selectedId || S.regions[S.selectedId]?.role !== 'ground') return;
  S.drawMode = check ? 'check' : 'calib';
  S.draft = [];
  $('#draw-hint').textContent = check ? '在画面上点击一个检查点位置。' : '在画面上点击一个新的标定点位置。';
  updateDrawButtons();
}

function updateDrawButtons() {
  const drawing = Boolean(S.drawMode && !['calib','check'].includes(S.drawMode));
  const placing = Boolean(S.drawMode && ['calib','check'].includes(S.drawMode));
  $('#draw-finish').disabled = !drawing;
  $('#draw-undo').disabled = !drawing || !S.draft.length;
  $('#draw-cancel').disabled = !drawing && !placing;
  $('#draw-region').disabled = drawing || placing;
  $('#draw-line').disabled = drawing || placing;
  $('#draw-ground').disabled = drawing || placing;
  $('#draw-gzone').disabled = drawing || placing;
}

function changeRegionRole(entry, role) {
  if (entry.kind !== 'region') return;
  const wasGround = entry.item.role === 'ground';
  if (role === 'ground' && entry.item.points?.length !== 4) {
    toast('地面标定必须正好 4 个点，请重新画一个四边形', 'error');
    renderRegionInspector(); return;
  }
  if (wasGround && role !== 'ground' && Object.hasOwn(rulesFor(entry), 'locate')) {
    toast('已切换区域用途并取消地面定位绑定，可在功能绑定中重新选择其它功能', 'ok');
  }
  const lostBindings = role === 'ground' && !wasGround &&
    Object.keys(rulesFor(entry)).some(key =>
      ['intrusion','fence','dwell'].includes(key) && ruleEnabled(entry.item, key));
  entry.item.role = role;
  if (role === 'ground') {
    entry.item.type = 'quad'; entry.item.points = orderGroundQuad(entry.item.points);
    entry.item.real_size_m = entry.item.real_size_m || [4, 8];
    delete entry.item.calib_points; delete entry.item.check_points;
  } else {
    entry.item.type = 'polygon';
    for (const key of ['real_size_m','calib_points','check_points']) delete entry.item[key];
  }
  const rules = entry.item.rules || {};
  for (const key of Object.keys(rules)) {
    if (!regionBindingAllowed(key, role)) delete rules[key];
  }
  if (role === 'ground') rules.locate = {};
  markDirty(); renderRegionList(); renderRegionInspector(); renderRegionCanvas();
  if (lostBindings) toast('已切换为地面标定，原告警区域绑定已取消', 'ok');
}

function rulesFor(entry) {
  return entry?.item?.rules && typeof entry.item.rules === 'object' ? entry.item.rules : {};
}

function changeGroundZoneSource(entry, source) {
  if (entry.kind !== 'ground_zone' || !['image', 'meters'].includes(source)) return;
  if (source === 'meters') {
    const calib = S.ground?.calib || {};
    const points = Array.isArray(entry.item.points_img) ? entry.item.points_img.map(p => imageToGround(calib.h, p)) : [];
    if (points.length < 3 || !points.every(validPoint)) {
      toast('没有可用的地面标定，无法转换成米制坐标；请先完成地面标定', 'error');
      renderRegionInspector();
      return;
    }
    entry.item.points_m = points.map(p => [round3(p[0]), round3(p[1])]);
    delete entry.item.points_img;
  } else {
    const points = Array.isArray(entry.item.points_m) ? entry.item.points_m : [];
    if (points.length < 3) {
      toast('地面区域至少需要 3 个点', 'error');
      renderRegionInspector();
      return;
    }
    delete entry.item.points_m;
    entry.item.points_img = fitPointsToImage(points);
  }
  markDirty(); renderRegionInspector(); renderRegionCanvas();
}

function fitPointsToImage(points) {
  const x0 = Math.min(...points.map(p => Number(p[0])));
  const x1 = Math.max(...points.map(p => Number(p[0])));
  const y0 = Math.min(...points.map(p => Number(p[1])));
  const y1 = Math.max(...points.map(p => Number(p[1])));
  const pad = .15;
  return points.map(p => [
    round4(pad + (x1 > x0 ? (Number(p[0]) - x0) / (x1 - x0) : .5) * (1 - pad * 2)),
    round4(pad + (y1 > y0 ? (Number(p[1]) - y0) / (y1 - y0) : .5) * (1 - pad * 2))
  ]);
}

function removeGroundZonePoint(entry, index) {
  const key = Array.isArray(entry.item.points_m) ? 'points_m' : 'points_img';
  const points = entry.item[key] || [];
  if (!Number.isInteger(index) || index < 0 || index >= points.length) return;
  if (points.length <= 3) {
    toast('地面区域至少需要 3 个点', 'error');
    return;
  }
  points.splice(index, 1);
  markDirty(); renderRegionInspector(); renderRegionCanvas();
}

function changeGroundMode(entry, mode) {
  if (entry.kind !== 'region' || entry.item.role !== 'ground') return;
  const pointsMode = isGroundPoints(entry.item);
  if (mode === 'points' && !pointsMode) {
    const [w, d] = entry.item.real_size_m || [4, 8];
    const pts = orderGroundQuad(entry.item.points || []);
    const ground = [[0,0],[w,0],[w,d],[0,d]];
    entry.item.calib_points = pts.map((p, i) => [round4(p[0]), round4(p[1]), ground[i][0], ground[i][1]]);
    entry.item.type = 'points';
    delete entry.item.real_size_m;
    entry.item.points = convexHull(entry.item.calib_points.map(c => c.slice(0, 2)));
  } else if (mode === 'rect' && pointsMode) {
    const calib = entry.item.calib_points || [];
    const hull = convexHull(calib.map(c => c.slice(0, 2)));
    entry.item.points = hull.length === 4 ? orderGroundQuad(hull) : orderGroundQuad(entry.item.points || []);
    let w = 4, d = 8;
    if (calib.length >= 2) {
      w = Math.max(...calib.map(c => c[2])) - Math.min(...calib.map(c => c[2])) || 4;
      d = Math.max(...calib.map(c => c[3])) - Math.min(...calib.map(c => c[3])) || 8;
    }
    entry.item.type = 'quad'; entry.item.real_size_m = [round3(w), round3(d)];
    delete entry.item.calib_points;
  }
  markDirty(); renderRegionInspector(); renderRegionCanvas();
}

function updateCalibHull(region) {
  const hull = convexHull((region.calib_points || []).map(c => c.slice(0, 2)));
  if (hull.length >= 3) region.points = hull.map(p => [round4(p[0]), round4(p[1])]);
}

function orderGroundQuad(input) {
  const pts = input.filter(validPoint).map(p => [Number(p[0]), Number(p[1])]);
  if (pts.length !== 4) return pts;
  const sortedY = [...pts].sort((a,b) => b[1] - a[1]);
  const near = sortedY.slice(0,2).sort((a,b) => a[0] - b[0]);
  const far = sortedY.slice(2).sort((a,b) => a[0] - b[0]);
  return [near[0], near[1], far[1], far[0]];
}

function convexHull(points) {
  const pts = points.filter(validPoint).map(p => [Number(p[0]), Number(p[1])]).sort((a,b) => a[0]-b[0] || a[1]-b[1]);
  if (pts.length <= 1) return pts;
  const cross = (o,a,b) => (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0]);
  const lower = [];
  for (const p of pts) { while (lower.length >= 2 && cross(lower.at(-2), lower.at(-1), p) <= 0) lower.pop(); lower.push(p); }
  const upper = [];
  for (let i = pts.length - 1; i >= 0; i--) { const p = pts[i]; while (upper.length >= 2 && cross(upper.at(-2), upper.at(-1), p) <= 0) upper.pop(); upper.push(p); }
  lower.pop(); upper.pop();
  return lower.concat(upper);
}

function selfIntersects(pts) {
  const n = pts.length;
  if (n < 4) return false;
  const cross = (a,b,c) => (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0]);
  for (let i = 0; i < n; i++) {
    const a1 = pts[i], a2 = pts[(i+1)%n];
    for (let j = i+2; j < n; j++) {
      if (i === 0 && j === n-1) continue;
      const b1 = pts[j], b2 = pts[(j+1)%n];
      if (cross(a1,a2,b1) * cross(a1,a2,b2) < 0 && cross(b1,b2,a1) * cross(b1,b2,a2) < 0) return true;
    }
  }
  return false;
}

function round4(v) { return Math.round(Number(v) * 10000) / 10000; }
function round3(v) { return Math.round(Number(v) * 1000) / 1000; }

function deleteSelectedRegion() {
  const entry = findRegionItem();
  if (!entry) return;
  if (!confirm(`删除“${entry.item.name || entry.id}”？删除后需要点击“保存区域”才会写入检测服务。`)) return;
  delete S.regions[entry.id]; delete S.lines[entry.id]; delete S.groundZones[entry.id];
  S.selectedId = null; S.dirty = true;
  renderRegionList(); renderRegionInspector(); renderRegionCanvas(); updateSaveButton();
}

function validateRegions() {
  const errors = [];
  if (Object.keys(S.regions).length > MAX_REGIONS) {
    errors.push(`区域最多只能有 ${MAX_REGIONS} 个`);
  }
  for (const [id, r] of Object.entries(S.regions)) {
    const name = r.name || id;
    const pts = (r.points || []).filter(validPoint);
    if (r.role === 'ground' && isGroundPoints(r)) {
      const calib = r.calib_points || [];
      if (calib.length < 4) errors.push(`地面标定“${name}”至少需要 4 个标定点`);
      if (calib.some(c => !Array.isArray(c) || c.length !== 4 ||
          c.some(v => !Number.isFinite(Number(v))))) {
        errors.push(`地面标定“${name}”的标定点格式不对`);
      }
      if (selfIntersects(pts)) errors.push(`地面标定“${name}”的轮廓自相交`);
    } else {
      if (pts.length < 3) errors.push(`区域“${name}”至少需要 3 个点`);
      if (selfIntersects(pts)) errors.push(`区域“${name}”的边自相交`);
      if (r.role === 'ground') {
        if (pts.length !== 4) errors.push(`地面标定“${name}”必须正好 4 个点`);
        if (!Array.isArray(r.real_size_m) || r.real_size_m.length < 2 || Number(r.real_size_m[0]) <= 0 || Number(r.real_size_m[1]) <= 0) {
          errors.push(`地面标定“${name}”需要填写实际宽度和纵深`);
        }
      }
    }
    for (const key of Object.keys(r.rules || {})) {
      if (REGION_FEATURE_KEYS.has(key) && !regionBindingAllowed(key, r.role || '')) {
        errors.push(`${FEATURE_NAMES[key]}不能绑定到“${name}”`);
      }
    }
  }
  for (const [id, l] of Object.entries(S.lines)) {
    if (!validPoint(l.p1) || !validPoint(l.p2) || Math.hypot(l.p1[0]-l.p2[0], l.p1[1]-l.p2[1]) < .02) errors.push(`进出线“${l.name || id}”太短`);
    for (const key of Object.keys(l.rules || {})) if (key !== 'count') errors.push(`“${l.name || id}”不能绑定功能 ${key}`);
  }
  for (const [id, z] of Object.entries(S.groundZones)) {
    const pts = (z.points_m || z.points_img || []).filter(validPoint);
    const hasM = Array.isArray(z.points_m), hasImg = Array.isArray(z.points_img);
    if (hasM === hasImg) errors.push(`地面区域“${z.name || id}”必须且只能有米制或画面坐标之一`);
    if (pts.length < 3) errors.push(`地面区域“${z.name || id}”至少需要 3 个点`);
    if (selfIntersects(pts)) errors.push(`地面区域“${z.name || id}”的边自相交`);
    if (Number(z.dwell_sec || 0) < 0) errors.push(`地面区域“${z.name || id}”的停留时间无效`);
  }
  for (const key of ['intrusion','fence','dwell','count','locate']) {
    if (featureEnabled(key) && featureBoundCount(key) === 0) errors.push(`${FEATURE_NAMES[key]}已开启，请先绑定${key === 'count' ? '至少一条进出线' : '至少一个区域'}`);
  }
  const locateCount = Object.values(S.regions).filter(r => r.role === 'ground' && ruleEnabled(r, 'locate')).length;
  if (locateCount > 1) errors.push('地面定位只能使用一个地面标定区域');
  return [...new Set(errors)];
}

async function saveRegions() {
  if (S.saving) return;
  const errors = validateRegions();
  if (errors.length) {
    toast(errors[0] + (errors.length > 1 ? `（还有 ${errors.length - 1} 处）` : ''), 'error');
    return;
  }
  if (S.drawMode) { toast('请先完成或取消正在画的形状', 'error'); return; }
  S.saving = true;
  const button = $('#regions-save');
  if (button) button.disabled = true;
  try {
    const viewKey = affineKey(S.status?.view?.applied);
    const latest = await api('/config');
    const next = deepMerge(latest, {});
    next.regions = clone(S.regions);
    next.lines = clone(S.lines);
    next.ground_zones = clone(S.groundZones);
    next.features = next.features || {};
    for (const key of ['intrusion','fence','dwell','count','locate']) {
      if (!next.features[key] || typeof next.features[key] !== 'object') next.features[key] = {};
      delete next.features[key].region_id;
      delete next.features[key].region_ids;
      delete next.features[key].line_ids;
    }
    for (const r of Object.values(next.regions)) {
      if (r.rules && !Object.keys(r.rules).length) delete r.rules;
    }
    for (const l of Object.values(next.lines)) {
      if (l.rules && !Object.keys(l.rules).length) delete l.rules;
    }
    for (const z of Object.values(next.ground_zones)) {
      if (Array.isArray(z.points_m)) delete z.points_img;
      else if (Array.isArray(z.points_img)) delete z.points_m;
      z.dwell_sec = Number(z.dwell_sec || 0);
      z.groups = Array.isArray(z.groups) ? z.groups : [];
    }
    const res = await api('/config', {method: 'PUT', body: next});
    S.config = next; S.dirty = false;
    // Saving makes the current view the reference. Keep the already rebased
    // coordinates and ignore the old correction in subsequent status polls.
    S.regionView = null;
    S.regionViewKey = viewKey || S.regionViewKey || '__saved__';
    const need = res.restart_required || [];
    toast(need.length ? `区域已保存；需重启：${need.join(', ')}` : '区域、进出线和地面区域已保存', 'ok');
    renderRegionList(); renderRegionInspector(); renderRegionCanvas();
  } catch (err) { toast(`保存失败: ${err.message}`, 'error'); }
  finally {
    S.saving = false;
    updateSaveButton();
  }
}

async function loadHeat(force = false) {
  if (!force && S.heat && Date.now() - (S.heat._at || 0) < 4000) return S.heat;
  try {
    S.heat = await api('/heatmap');
    S.heat._at = Date.now();
    renderHeatOverlay(); renderRegionCanvas();
    return S.heat;
  } catch (err) { return null; }
}

function renderHeatOverlay() {
  const canvas = $('#heat-layer');
  if (!canvas) return;
  const visible = S.showHeatmap || Boolean(getPath(S.config, 'display.show_heatmap', false));
  canvas.classList.toggle('hidden', !visible);
  const img = $('#live-stream');
  if (!visible || !S.heat) return;
  const w = Math.max(1, img.clientWidth), h = Math.max(1, img.clientHeight);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  drawHeatGrid(ctx, S.heat, w, h, 0.55);
}

async function suggestFromHeat() {
  const heat = await loadHeat(true);
  if (!heat) { toast('还没有热图数据', 'error'); return; }
  const cols = Number(heat.cols || 64), rows = Number(heat.rows || 36);
  const values = new Float32Array(cols * rows);
  let max = 0;
  for (let i = 0; i < values.length; i++) {
    values[i] = Number(heat.person?.[i] || 0) + Number(heat.vehicle?.[i] || 0);
    max = Math.max(max, values[i]);
  }
  if (max < 5) { toast('热图样本太少，继续统计后再试', 'error'); return; }
  const threshold = max * .18;
  const seen = new Uint8Array(values.length);
  const components = [];
  for (let y = 0; y < rows; y++) for (let x = 0; x < cols; x++) {
    const idx = y * cols + x;
    if (seen[idx] || values[idx] < threshold) continue;
    const stack = [[x,y]]; seen[idx] = 1;
    const cells = [];
    while (stack.length) {
      const [cx,cy] = stack.pop(); cells.push([cx,cy]);
      for (const [dx,dy] of [[1,0],[-1,0],[0,1],[0,-1]]) {
        const nx = cx+dx, ny = cy+dy, ni = ny*cols+nx;
        if (nx>=0 && nx<cols && ny>=0 && ny<rows && !seen[ni] && values[ni]>=threshold) { seen[ni]=1; stack.push([nx,ny]); }
      }
    }
    if (cells.length >= 5) components.push(cells);
  }
  components.sort((a,b) => b.length - a.length);
  if (!components.length) { toast('没有找到足够集中的热点，继续统计后再试', 'error'); return; }
  const available = Math.max(0, MAX_REGIONS - Object.keys(S.regions).length);
  if (!available) { toast(`区域最多只能有 ${MAX_REGIONS} 个；请先删除不用的区域`, 'error'); return; }
  const count = Math.min(3, components.length, available);
  const used = components.slice(0, count).map((cells, i) => {
    const xs = cells.map(c => c[0]), ys = cells.map(c => c[1]);
    const x0 = Math.max(0, Math.min(...xs) / cols - .02), x1 = Math.min(1, (Math.max(...xs) + 1) / cols + .02);
    const y0 = Math.max(0, Math.min(...ys) / rows - .02), y1 = Math.min(1, (Math.max(...ys) + 1) / rows + .02);
    const id = newId('region');
    S.regions[id] = {type:'polygon', name:`热区建议 ${i+1}`, role:'private',
      points:[[round4(x0),round4(y0)],[round4(x1),round4(y0)],[round4(x1),round4(y1)],[round4(x0),round4(y1)]], rules:{}};
    return id;
  });
  S.selectedId = used[0]; S.dirty = true;
  renderRegionList(); renderRegionInspector(); renderRegionCanvas(); updateSaveButton();
  const skipped = Math.min(3, components.length) - used.length;
  toast(`已根据热图生成 ${used.length} 个建议区域，请检查后绑定功能并保存` +
    (skipped > 0 ? `；区域已达 ${MAX_REGIONS} 个上限，未添加 ${skipped} 个建议` : ''), 'ok');
}

// ---------------------------------------------------------------- 鸟瞰图 / 地面
function bindGround() {
  $('#ground-reset').addEventListener('click', async () => {
    if (!confirm('清零所有地面区域的累计次数和停留时间？')) return;
    try { await api('/ground/stats', {method: 'DELETE'}); toast('地面统计已清零', 'ok'); loadGround(); }
    catch (err) { toast(`清零失败: ${err.message}`, 'error'); }
  });
  $('#geo-refresh').addEventListener('click', refreshGeo);
  $('#ground-add-zone').addEventListener('click', () => {
    const hull = S.ground?.calib?.hull_m || [[0,0],[3,0],[3,3],[0,3]];
    const xs = hull.map(p => Number(p[0])), ys = hull.map(p => Number(p[1]));
    const cx = (Math.min(...xs) + Math.max(...xs)) / 2 || 0, cy = (Math.min(...ys) + Math.max(...ys)) / 2 || 0;
    const id = newId('gz');
    S.groundZones[id] = {name:`地面区域 ${Object.keys(S.groundZones).length + 1}`,
      points_m:[[cx-1,cy-1],[cx+1,cy-1],[cx+1,cy+1],[cx-1,cy+1]],
      enabled:true, alarm_enter:false, dwell_sec:0, groups:[]};
    S.selectedId = id; S.dirty = true;
    showView('regions'); renderRegionList(); renderRegionInspector(); renderRegionCanvas(); updateSaveButton();
    toast('已新建地面区域，请检查坐标、告警参数并保存', 'ok');
  });
  window.addEventListener('resize', () => { renderBev(); renderRegionCanvas(); renderHeatOverlay(); });
}

async function refreshGeo() {
  const button = $('#geo-refresh');
  if (S.geoLocating || button?.disabled) return;
  if (S.geo?.enabled === false || S.config?.geo?.enabled === false) {
    toast('请先在「设置 > 地面定位」中开启自动获取设备经纬度', 'error');
    return;
  }
  const beforeAttempt = String(S.geo?.last_attempt || '');
  S.geoLocating = true;
  renderGeoSummary();
  try {
    await api('/geo/refresh', {method: 'POST'});
    toast('正在重新定位', 'ok');
    const deadline = Date.now() + 60000;
    while (Date.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      await loadGeo();
      const geo = S.geo || {};
      if (geo.last_attempt && String(geo.last_attempt) !== beforeAttempt) {
        const attempts = geo.attempts && typeof geo.attempts === 'object' ? Object.values(geo.attempts) : [];
        const succeeded = attempts.length ? attempts.includes('ok') : !!geo.valid;
        toast(succeeded ? '定位完成' : '仍未获取到设备位置', succeeded ? 'ok' : 'error');
        return;
      }
    }
    toast('定位超时，请检查定位来源和网络', 'error');
  } catch (err) {
    toast(`${translateExact('定位失败')}: ${err.message}`, 'error');
  } finally {
    S.geoLocating = false;
    renderGeoSummary();
  }
}

async function loadGround() {
  return withBusy('ground', async () => {
    try {
      S.ground = await api('/ground');
      renderGround();
    } catch (err) {
      $('#bev-caption').textContent = err.message;
    }
  });
}

function renderGround() {
  renderBev();
  const g = S.ground || {};
  const calib = g.calib || {};
  const quality = calib.quality || {};
  const bevTargets = g.targets?.length || 0;
  const bevZones = g.zones?.length || 0;
  $('#bev-caption').textContent = currentLanguage === 'en'
    ? (g.enabled ? `Localization running · ${bevTargets} targets · ${bevZones} ground zones` : 'Ground localization is off')
    : (g.enabled ? `定位运行中 · ${bevTargets} 个目标 · ${bevZones} 个地面区域` : '地面定位未启用');
  $('#calib-grade').textContent = quality.grade ? ({good:'良好', fair:'一般', poor:'较差'})[quality.grade] || quality.grade : (calib.valid ? '有效' : '未标定');
  $('#ground-summary').innerHTML = details([
    ['定位状态', g.enabled ? '运行中' : '未启用'],
    ['标定区域', calib.name || '—'],
    ['标定方式', calib.mode === 'points' ? '多点对应' : '矩形 4 点'],
    ['标定点', String(quality.n_points ?? '—')],
    ['拟合残差', quality.rmse_m != null ? `${fmt(quality.rmse_m, 3)} m` : '—'],
    ['检查误差', quality.check_rmse_m != null ? `${fmt(quality.check_rmse_m, 3)} m` : '—'],
    ['覆盖率', quality.coverage != null ? pct(quality.coverage, 1) : '—'],
    ['运行健康', g.runtime?.health || '—']
  ]);
  const liveZones = Object.fromEntries((g.zones || []).map(z => [z.id, z]));
  const rows = Object.entries(S.groundZones);
  $('#ground-zone-list').innerHTML = rows.length ? rows.map(([id, z]) => {
    const live = liveZones[id] || {};
    const stats = live.stats || {};
    return `<button class="simple-row zone-button" data-ground-zone="${esc(id)}" style="border:0;text-align:left;width:100%;cursor:pointer"><span><strong>${esc(z.name || id)}</strong><br><small>当前 ${stats.count_now ?? 0} · 累计 ${stats.visits ?? 0} 次 · 平均 ${fmt(stats.avg_dwell_sec, 1)} 秒</small></span><em>${z.enabled === false ? '停用' : z.alarm_enter ? '告警' : '记录'}</em></button>`;
  }).join('') : '<span class="muted">还没有地面区域</span>';
  $$('[data-ground-zone]').forEach(btn => btn.addEventListener('click', () => {
    S.selectedId = btn.dataset.groundZone;
    showView('regions');
    renderRegionList(); renderRegionInspector(); renderRegionCanvas();
  }));
  const targets = Array.isArray(g.targets) ? g.targets : [];
  $('#ground-target-count').textContent = String(targets.length);
  $('#ground-targets').innerHTML = targets.length ? targets.slice(0, 12).map(t => `
    <div class="simple-row"><span><strong>#${esc(t.id)} ${esc(t.label || '')}</strong><br><small>${fmt(t.x, 2)}, ${fmt(t.y, 2)} m · ${fmt(t.speed, 2)} m/s</small></span><em>${esc(t.group || '')}</em></div>`).join('') : '<span class="muted">当前没有目标</span>';
}

function renderBev() {
  const canvas = $('#bev-canvas');
  if (!canvas) return;
  const rect = canvas.getBoundingClientRect();
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.max(400, rect.width), h = Math.max(420, rect.height || 520);
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#0c1015'; ctx.fillRect(0, 0, w, h);
  const g = S.ground || {};
  const calib = g.calib || {};
  const zones = groundZonesMeters();
  const targets = (g.targets || []).filter(t => Number.isFinite(Number(t.x)) && Number.isFinite(Number(t.y)));
  const all = [...(calib.hull_m || []), ...zones.flatMap(z => z.points), ...targets.filter(t => t.in_calib !== false).map(t => [t.x,t.y])];
  const bounds = padBounds(pointsBounds(all), .6);
  const view = {};
  const margin = 34;
  const sx = (w - margin * 2) / Math.max(.1, bounds.x1 - bounds.x0);
  const sy = (h - margin * 2) / Math.max(.1, bounds.y1 - bounds.y0);
  const scale = Math.min(sx, sy);
  const ox = (w - (bounds.x1 - bounds.x0) * scale) / 2;
  const oy = (h - (bounds.y1 - bounds.y0) * scale) / 2;
  view.toScreen = (x,y) => [ox + (x - bounds.x0) * scale, h - oy - (y - bounds.y0) * scale];
  drawBevGrid(ctx, bounds, view, w, h);
  if ((calib.hull_m || []).length >= 3) {
    polygonPath(ctx, calib.hull_m, view);
    ctx.fillStyle = 'rgba(94,168,255,.06)'; ctx.fill();
    ctx.strokeStyle = 'rgba(120,187,255,.65)'; ctx.lineWidth = 1.5; ctx.setLineDash([7,5]); ctx.stroke(); ctx.setLineDash([]);
  }
  for (const z of zones) {
    if (z.points.length < 3) continue;
    polygonPath(ctx, z.points, view);
    ctx.fillStyle = hexToRgba(z.color, z.count ? .32 : .13); ctx.fill();
    ctx.strokeStyle = z.enabled ? z.color : hexToRgba(z.color, .45);
    ctx.lineWidth = z.count ? 3 : 1.8; ctx.stroke();
    const c = centroid(z.points);
    if (c) {
      const [px,py] = view.toScreen(c[0],c[1]);
      ctx.font = '600 11px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif'; ctx.textAlign = 'center'; ctx.fillStyle = '#fff';
      ctx.fillText(`${z.name}${z.count ? ` · ${z.count}` : ''}`, px, py);
      ctx.textAlign = 'start';
    }
  }
  for (const t of targets) {
    const trail = (t.trail || []).filter(validPoint);
    if (trail.length > 1) {
      ctx.beginPath();
      const [tx0,ty0] = view.toScreen(trail[0][0],trail[0][1]); ctx.moveTo(tx0,ty0);
      for (const p of trail.slice(1)) { const [px,py] = view.toScreen(p[0],p[1]); ctx.lineTo(px,py); }
      const [tx,ty] = view.toScreen(t.x,t.y); ctx.lineTo(tx,ty);
      ctx.strokeStyle = t.in_calib === false ? 'rgba(160,170,180,.35)' : 'rgba(68,209,123,.48)';
      ctx.lineWidth = 1.7; ctx.stroke();
    }
    const [px,py] = view.toScreen(t.x,t.y);
    const color = t.group === 'vehicle' ? '#f0b84b' : t.group === 'pet' ? '#a989ff' : '#44d17b';
    ctx.beginPath(); ctx.arc(px,py,6,0,Math.PI*2);
    if (t.in_calib === false) { ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke(); }
    else { ctx.fillStyle = color; ctx.fill(); ctx.strokeStyle = '#0c1015'; ctx.lineWidth = 1.5; ctx.stroke(); }
    if (t.has_heading && Number.isFinite(Number(t.heading_deg))) {
      const a = Number(t.heading_deg) * Math.PI / 180;
      const dx = Math.sin(a), dy = -Math.cos(a);
      ctx.beginPath(); ctx.moveTo(px+dx*7,py+dy*7); ctx.lineTo(px+dx*23,py+dy*23); ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
      ctx.beginPath(); ctx.moveTo(px+dx*23,py+dy*23); ctx.lineTo(px+dx*16+dy*5,py+dy*16-dx*5); ctx.lineTo(px+dx*16-dy*5,py+dy*16+dx*5); ctx.stroke();
    }
    ctx.font = '11px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif'; ctx.fillStyle = '#fff';
    ctx.fillText(`#${t.id || '?'} ${t.label || ''}`, px+9, py-8);
  }
  const quality = calib.quality || {};
  ctx.font = '12px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif'; ctx.fillStyle = '#c9d3df';
  const gradeMap = {good: 'Good', fair: 'Fair', poor: 'Poor'};
  const grade = gradeMap[quality.grade] || '';
  ctx.fillText(calib.valid
    ? (currentLanguage === 'en'
      ? `Calibration ${grade || 'valid'} · ${fmt(quality.rmse_m, 2)} m`
      : `标定 ${grade || '有效'} · ${fmt(quality.rmse_m, 2)} m`)
    : (currentLanguage === 'en' ? 'Ground calibration incomplete' : '未完成地面标定'), 14, 20);
  ctx.fillStyle = '#6f7b8c'; ctx.font = '10px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif';
  ctx.fillText(currentLanguage === 'en' ? 'x right · y away · meters' : 'x 向右 · y 向远处 · 单位：米', 14, h - 12);
  $('#ground-legend').innerHTML = `<span class="legend-item"><i class="legend-dot" style="background:#30d158"></i>人员</span>
    <span class="legend-item"><i class="legend-dot" style="background:#f0b84b"></i>车辆</span>
    <span class="legend-item"><i class="legend-dot" style="background:#a989ff"></i>宠物</span>
    <span class="legend-item"><i class="legend-dot" style="background:#5ea8ff"></i>地面标定范围</span>`;
}

function groundZonesMeters() {
  const calib = S.ground?.calib || {};
  const live = Object.fromEntries((S.ground?.zones || []).map(z => [z.id, z]));
  return Object.entries(S.groundZones).map(([id, z]) => {
    let pts = [];
    if (Array.isArray(z.points_m)) pts = z.points_m.map(p => [Number(p[0]),Number(p[1])]);
    else if (Array.isArray(z.points_img)) {
      pts = (z.points_img || []).map(p => imageToGround(calib.h, p)).filter(Boolean);
      if (!pts.length && live[id]?.points_m) pts = live[id].points_m;
    }
    return {id, name:z.name || id, points:pts, color:zoneColor(id), enabled:z.enabled !== false,
      count:Number(live[id]?.stats?.count_now || 0)};
  });
}

function imageToGround(h, p) {
  if (!Array.isArray(h) || h.length !== 9 || !validPoint(p)) return null;
  const u = Number(p[0]), v = Number(p[1]);
  const den = h[6]*u + h[7]*v + h[8];
  if (Math.abs(den) < 1e-9) return null;
  return [(h[0]*u + h[1]*v + h[2]) / den, (h[3]*u + h[4]*v + h[5]) / den];
}

function pointsBounds(points) {
  const valid = points.filter(validPoint);
  if (!valid.length) return {x0:-1, y0:-1, x1:1, y1:1};
  return {x0:Math.min(...valid.map(p=>Number(p[0]))), y0:Math.min(...valid.map(p=>Number(p[1]))),
    x1:Math.max(...valid.map(p=>Number(p[0]))), y1:Math.max(...valid.map(p=>Number(p[1])))};
}

function padBounds(b, pad) {
  if (b.x1 - b.x0 < 1) { const c=(b.x0+b.x1)/2; b.x0=c-.5; b.x1=c+.5; }
  if (b.y1 - b.y0 < 1) { const c=(b.y0+b.y1)/2; b.y0=c-.5; b.y1=c+.5; }
  return {x0:b.x0-pad, y0:b.y0-pad, x1:b.x1+pad, y1:b.y1+pad};
}

function polygonPath(ctx, points, view) {
  ctx.beginPath();
  const [x0,y0] = view.toScreen(points[0][0],points[0][1]); ctx.moveTo(x0,y0);
  for (const p of points.slice(1)) { const [x,y] = view.toScreen(p[0],p[1]); ctx.lineTo(x,y); }
  ctx.closePath();
}

function drawBevGrid(ctx, bounds, view, w, h) {
  const span = Math.max(bounds.x1-bounds.x0, bounds.y1-bounds.y0);
  const step = span > 30 ? 10 : span > 12 ? 5 : span > 5 ? 2 : 1;
  ctx.lineWidth = 1;
  for (let x = Math.ceil(bounds.x0/step)*step; x <= bounds.x1; x += step) {
    const [sx] = view.toScreen(x,0);
    ctx.strokeStyle = Math.abs(x) < 1e-6 ? 'rgba(255,255,255,.25)' : 'rgba(255,255,255,.065)';
    ctx.beginPath(); ctx.moveTo(sx,0); ctx.lineTo(sx,h); ctx.stroke();
    ctx.fillStyle = 'rgba(150,160,175,.75)'; ctx.font = '10px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif'; ctx.fillText(`${x}m`, sx+3, h-18);
  }
  for (let y = Math.ceil(bounds.y0/step)*step; y <= bounds.y1; y += step) {
    const [,sy] = view.toScreen(0,y);
    ctx.strokeStyle = Math.abs(y) < 1e-6 ? 'rgba(255,255,255,.25)' : 'rgba(255,255,255,.065)';
    ctx.beginPath(); ctx.moveTo(0,sy); ctx.lineTo(w,sy); ctx.stroke();
    ctx.fillStyle = 'rgba(150,160,175,.75)'; ctx.font = '10px "Monitor Box Sans", "Monitor Box Symbols", "Noto Sans SC", "Noto Sans", "WenQuanYi Zen Hei", sans-serif'; ctx.fillText(`${y}m`, 5, sy-4);
  }
}

// ---------------------------------------------------------------- 启动
function initApp() {
  document.body.dataset.view = S.view;
  document.documentElement.dataset.view = S.view;
  setLanguage('zh_CN', {rerender: false});
  new MutationObserver(scheduleLocalize).observe(document.body, {
    childList: true, subtree: true, characterData: true,
    attributes: true, attributeFilter: ['title', 'aria-label', 'placeholder', 'alt']
  });
  let savedTheme = 'light';
  try { savedTheme = localStorage.getItem('monitor-box-theme') || 'light'; } catch (_) {}
  applyTheme(savedTheme);
  bindNavigation();
  bindStatusActions();
  bindSettings();
  bindEditor();
  bindGround();
  const stream = $('#live-stream');
  stream.addEventListener('error', () => {
    $('#live-state').textContent = '画面断开';
    setTimeout(() => { stream.src = `/stream?t=${Date.now()}`; }, 1400);
  });
  stream.addEventListener('load', () => { $('#live-state').textContent = '在线'; });
  $('#live-stream').addEventListener('load', renderHeatOverlay);
  renderFeatureSwitches();
  loadConfig();
  loadStatus();
  loadEvents(true);
  loadGeo();
  setInterval(loadStatus, 2000);
  setInterval(() => {
    if (S.view === 'live') loadEvents(false);
    else if (S.view === 'events' && S.eventPages === 0) loadEvents(false);
  }, 4500);
  setInterval(() => {
    if (S.view === 'ground') loadGround();
  }, 900);
  setInterval(() => {
    const on = S.view === 'regions' ? $('#show-heatmap')?.checked : S.showHeatmap;
    if (on) loadHeat();
  }, 5000);
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initApp);
else initApp();
