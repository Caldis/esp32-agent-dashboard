// app.js — ESP32 agent dashboard web dev-tools panel.
//
// Purpose: DATA + INTERACTION symmetry with the firmware, NOT rendering its
// screen. Every real `dash` line (from a live agent's hooks OR from /inject)
// arrives over SSE and is fed to the SAME firmware data layer compiled to WASM,
// so the browser computes `state_json` exactly like the device — same parsing,
// same bugs. The panel lists that data and lets you drive input back through
// the real bridge (approve/deny, quick-reply, inject events).
//
// Link: CC/Codex hook -> hook_dispatch -> bridge -> serve.py(device) -> SSE.
//       Or: this panel POST /inject -> bridge -> ... -> SSE (no agent needed).
import DashDataLayer from './dash_datalayer.mjs';

// ── WASM same-source data layer ──────────────────────────────────────────────
const M = await DashDataLayer();
const dash_init     = M.cwrap('dash_init', null, []);
const dash_feed     = M.cwrap('dash_feed_line', 'number', ['string']);
const state_json    = M.cwrap('state_json', 'string', []);
const current_scene = M.cwrap('current_scene', 'string', []);
const drain_signals = M.cwrap('drain_signals', 'string', []);
dash_init();

// ── DOM refs ─────────────────────────────────────────────────────────────────
const $ = (id) => document.getElementById(id);
const elConnDot = $('conn-dot'), elConnText = $('conn-text');
const elDevice = $('h-device'), elScene = $('h-scene'), elBridge = $('h-bridge');
const elLink = $('h-link'), elClients = $('h-clients'), elMode = $('h-mode');
const elTotals = $('totals'), elAwaiting = $('awaiting'), elAgents = $('agents');
const elAgentsCount = $('agents-count');
const elStateJson = $('statejson'), elFrames = $('frames'), elFramesCount = $('frames-count');
const elSignals = $('signals'), elHooks = $('hooks');

// ── local state ──────────────────────────────────────────────────────────────
let connected = false;
let currentPrompt = null;      // latest `dash prompt` payload {id, tool, hint, mode?}
let frameN = 0;
const frames = [];             // {t, verb, line}
const signals = [];            // {t, sig}
const MAX = 250;

const esc = (s) => String(s ?? '').replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const now = () => new Date().toLocaleTimeString();

function parsePromptPayload(line) {
  const a = line.indexOf('{'), b = line.lastIndexOf('}');
  if (a < 0 || b < a) return null;
  try { return JSON.parse(line.slice(a, b + 1)); } catch { return null; }
}

// ── render ───────────────────────────────────────────────────────────────────
function render() {
  let s;
  try { s = JSON.parse(state_json()); }
  catch (e) { elStateJson.textContent = 'state_json parse error: ' + e; return; }

  elScene.textContent = current_scene() || '(none)';
  elDevice.textContent = s.device_name || '—';

  // totals → stat strip
  const t = s.totals || {};
  const stat = (n, l, cls = '') => `<div class="stat"><div class="n ${cls}">${n}</div><div class="l">${l}</div></div>`;
  const fmt = (n) => (n ?? 0).toLocaleString('en-US');
  elTotals.innerHTML =
    stat(fmt(t.total), 'agents') +
    stat(fmt(t.running), 'running', t.running ? 'ok' : '') +
    stat(fmt(t.waiting), 'waiting', t.waiting ? 'warn' : '') +
    stat(fmt(t.tokens), 'tokens') +
    stat(fmt(t.tokens_today), 'today');

  // agents → cards
  const slots = s.slots || [];
  elAgentsCount.textContent = slots.length ? `${slots.length} 活跃` : '';
  if (!slots.length) {
    elAgents.innerHTML = '<span class="muted">无活跃 agent</span>';
  } else {
    elAgents.innerHTML = slots.map(a => {
      const k = a.kind === 'claude-code' ? 'cc' : a.kind === 'codex' ? 'cx' : 'ag';
      const aw = a.awaiting && a.awaiting !== 'none' ? `<span class="aw">${esc(a.awaiting)}</span>` : '';
      return `<div class="agent">
        <span class="badge ${k}">${k}</span>
        <div class="grow">
          <div class="top"><span class="st st-${esc(a.status)}">${esc(a.status)}</span>${aw}
            <span class="sid">${esc(a.session_id).slice(0, 14)}</span></div>
          ${a.msg ? `<div class="msg">${esc(a.msg)}</div>` : ''}
          ${a.cwd ? `<div class="cwd">${esc(a.cwd)}</div>` : ''}
        </div>
        <div class="tok">${fmt(a.tokens)}<small>tok</small></div>
      </div>`;
    }).join('');
  }

  // awaiting interaction — only while the device reports prompt active
  const active = s.prompt && s.prompt.active;
  if (!active) currentPrompt = null;
  renderAwaiting(s);

  // device state machine (debug)
  renderDeviceState(s);

  // full state
  elStateJson.textContent = JSON.stringify(s, null, 2);
}

function renderAwaiting(s) {
  if (!currentPrompt) {
    elAwaiting.innerHTML = '<span class="muted">无待处理 prompt</span>';
    return;
  }
  const p = currentPrompt;
  const id = esc(p.id);
  if (p.mode === 'reply') {
    const opts = [p.tool, p.hint].filter(Boolean);
    elAwaiting.innerHTML =
      `<div class="takeover"><div class="hl">⌶ quick-reply</div>` +
      `<div class="meta"><span class="k">id</span><span class="mono">${id}</span></div>` +
      `<div class="row">` +
      opts.map((o, i) => `<button class="opt" data-reply="${i}">${i + 1}. ${esc(o)}</button>`).join('') +
      `</div></div>`;
    elAwaiting.querySelectorAll('button[data-reply]').forEach(b => {
      b.onclick = () => sendReply(p.id, +b.dataset.reply);
    });
  } else {
    elAwaiting.innerHTML =
      `<div class="takeover"><div class="hl">🔒 permission</div>` +
      `<div class="meta">` +
      `<span class="k">tool</span><span>${esc(p.tool)}</span>` +
      `<span class="k">hint</span><span class="mono">${esc(p.hint)}</span>` +
      `<span class="k">id</span><span class="mono">${id}</span></div>` +
      `<div class="row">` +
      `<button class="approve" data-dec="once">✓ Approve (once)</button>` +
      `<button class="deny" data-dec="deny">✕ Deny</button></div></div>`;
    elAwaiting.querySelectorAll('button[data-dec]').forEach(b => {
      b.onclick = () => sendDecision(p.id, b.dataset.dec);
    });
  }
}

// ── device state machine (debug) ─────────────────────────────────────────────
// Infers what the DEVICE is showing (zzz / thinking / your turn / pick / …) from
// the same mirror data, records each transition (which signal moved it, when),
// and draws the full state graph so you can see the current state, where it can
// go next, and why it did / didn't move. Mirrors the firmware's scene logic:
// esp32_main scene tick (awaiting) + agent_commands cmd_snapshot (idle/dashboard).
const elDevCur = $('dev-current'), elDevGraph = $('dev-graph'), elDevHist = $('dev-history');
let lastFedLine = '';
let devState = null;
const devHistory = [];

const DEV_STATES = {
  idle:     { label: 'zzz 睡眠',  cls: 's-idle',    desc: '无活跃 agent(total=0)' },
  thinking: { label: 'thinking',  cls: 's-running', desc: '有 agent 运行中;平时呼吸动画,有新工具活动自动显示明细 6s 再回落' },
  continue: { label: 'your turn', cls: 's-waiting', desc: 'agent 结束回合,球在你这边' },
  pick:     { label: 'pick 选项', cls: 's-waiting', desc: 'agent 给了编号选项(展示)' },
  type:     { label: 'type 输入', cls: 's-waiting', desc: 'agent 问了开放问题' },
  clarify:  { label: 'clarify',   cls: 's-waiting', desc: 'agent 请求澄清' },
  approve:  { label: 'approve',   cls: 's-waiting', desc: '权限请求(observe 模式少见)' },
  prompt:   { label: 'prompt',    cls: 's-waiting', desc: 'dash prompt 权限 takeover' },
};
// outgoing edges: what makes a state leave, and to where
const DEV_EDGES = {
  idle:     [{ to: 'thinking', cond: '收到 snapshot 且 total>0(首个 agent)' }],
  thinking: [{ to: 'continue', cond: 'Stop · 分类 continue' },
             { to: 'pick',     cond: 'Stop · 检测到编号选项' },
             { to: 'type',     cond: 'Stop · 问句结尾' },
             { to: 'clarify',  cond: 'Stop · 澄清关键词' },
             { to: 'idle',     cond: 'total→0(全部结束 / SessionEnd)' }],
  continue: [{ to: 'thinking', cond: '你提交 / agent 调工具(清 awaiting)' },
             { to: 'idle',     cond: 'total→0' }],
  pick:     [{ to: 'thinking', cond: '你提交 / 调工具' }, { to: 'idle', cond: 'total→0' }],
  type:     [{ to: 'thinking', cond: '你提交 / 调工具' }, { to: 'idle', cond: 'total→0' }],
  clarify:  [{ to: 'thinking', cond: '你提交 / 调工具' }, { to: 'idle', cond: 'total→0' }],
  approve:  [{ to: 'thinking', cond: '决策完成' }, { to: 'idle', cond: 'total→0' }],
  prompt:   [{ to: 'thinking', cond: 'prompt 清除(snapshot prompt:null)' }],
};

function deviceState(s) {
  const t = s.totals || {}, slots = s.slots || [];
  if (s.prompt && s.prompt.active) return 'prompt';
  const aw = slots.find(a => a.awaiting && a.awaiting !== 'none');
  if (aw) return DEV_STATES[aw.awaiting] ? aw.awaiting : 'continue';
  if ((t.total || 0) === 0) return 'idle';
  return 'thinking';
}

function renderDeviceState(s) {
  const key = deviceState(s);
  if (devState !== key) {
    const verb = lastFedLine ? (lastFedLine.split(/\s+/)[1] || '?') : '(初始)';
    // Semantic reason: the device only ever receives `snapshot`, so the raw verb
    // is uninformative. Use the matching from→to edge condition as the "why".
    let reason = verb === '(初始)' ? '初始状态' : '';
    if (devState && DEV_EDGES[devState]) {
      const e = DEV_EDGES[devState].find(x => x.to === key);
      if (e) reason = e.cond;
    }
    if (!reason) reason = verb + ' 信号';
    devHistory.push({ t: now(), from: devState, to: key, sig: verb, reason });
    if (devHistory.length > 200) devHistory.shift();
    devState = key;
  }
  const st = DEV_STATES[key] || { label: key, cls: '', desc: '' };
  const last = devHistory[devHistory.length - 1];
  const lbl = (k) => DEV_STATES[k] ? DEV_STATES[k].label : (k || '∅');

  const edges = (DEV_EDGES[key] || []).map(e =>
    `<div class="e">→ <b>${esc(lbl(e.to))}</b> <span class="ed">当 ${esc(e.cond)}</span></div>`).join('');
  elDevCur.innerHTML =
    `<div class="dev-now"><div class="name ${st.cls}">${esc(st.label)}</div>` +
    `<div class="by">${esc(st.desc)}</div>` +
    `<div class="by">触发原因 <b style="color:var(--teal)">${esc(last ? last.reason : '—')}</b>` +
    (last && last.from ? ` · 从 <b>${esc(lbl(last.from))}</b> 转入` : '') +
    (last ? ` · ${esc(last.t)}` : '') +
    (last && last.sig ? ` <span class="ed">(底层 ${esc(last.sig)})</span>` : '') + `</div>` +
    `<div class="next"><span class="muted">下一步可能:</span>${edges || ' (终态)'}</div></div>`;

  elDevGraph.innerHTML = '<div class="sm">' + Object.keys(DEV_STATES).map(k => {
    const node = DEV_STATES[k];
    const ed = (DEV_EDGES[k] || []).map(e => `<span class="ed">→ ${esc(lbl(e.to))}</span>`).join('');
    return `<div class="sm-node ${k === key ? 'on' : ''}"><div class="t ${node.cls}">` +
      `${esc(node.label)}${k === key ? '  ◀ 当前' : ''}</div>` +
      `<div class="d">${esc(node.desc)}</div>` +
      `<div class="edges">${ed || '<span class="ed">(终态)</span>'}</div></div>`;
  }).join('') + '</div>';

  elDevHist.innerHTML = devHistory.slice(-60).reverse().map(h =>
    `<div class="ln"><span class="t">${h.t}</span> <span class="body">${esc(lbl(h.from))} → ` +
    `<b>${esc(lbl(h.to))}</b> <span class="ed">${esc(h.reason || h.sig)}</span></span></div>`
  ).join('') || '<div class="empty">无</div>';
}

async function sendDecision(id, decision) {
  currentPrompt = null; render();
  await postJSON('/decision', { id, decision });
}
async function sendReply(id, choice) {
  currentPrompt = null; render();
  await postJSON('/reply', { id, choice });
}

// ── frames + signals logs ────────────────────────────────────────────────────
function renderFrames() {
  elFramesCount.textContent = `${frameN} 帧`;
  if (!frames.length) { elFrames.innerHTML = '<div class="empty">等待 hook/注入事件…</div>'; return; }
  elFrames.innerHTML = frames.slice(-150).reverse().map(f => {
    const body = f.line.replace(/^dash\s+\S+\s*/, '');   // strip "dash <verb> " prefix
    return `<div class="ln"><span class="t">${f.t}</span>` +
      `<span class="v v-${esc(f.verb)}">${esc(f.verb)}</span>` +
      `<span class="body">${esc(body.slice(0, 220)) || '·'}</span></div>`;
  }).join('');
}
function pushFrame(line) {
  const verb = line.split(/\s+/)[1] || '?';
  frames.push({ t: now(), verb, line });
  if (frames.length > MAX) frames.shift();
  frameN++;
  renderFrames();
}
function renderSignals() {
  if (!signals.length) { elSignals.innerHTML = '<div class="empty">无</div>'; return; }
  elSignals.innerHTML = signals.slice(-150).reverse().map(x =>
    `<div class="ln"><span class="t">${x.t}</span> <span class="sig">${esc(x.sig)}</span></div>`
  ).join('');
}
function pushSignals(arr) {
  for (const sig of arr) {
    signals.push({ t: now(), sig });
    if (signals.length > MAX) signals.shift();
  }
  renderSignals();
}
$('frames-clear').onclick = () => { frames.length = 0; frameN = 0; renderFrames(); };
$('signals-clear').onclick = () => { signals.length = 0; renderSignals(); };

// ── SSE: feed every real dash line to the WASM data layer ────────────────────
const es = new EventSource('/events');
es.onopen = () => { connected = true; setConn(); };
es.onerror = () => { connected = false; setConn(); };
es.onmessage = (ev) => {
  let line;
  try { line = JSON.parse(ev.data); } catch { line = ev.data; }
  if (typeof line !== 'string') return;
  pushFrame(line);
  lastFedLine = line;              // newest signal — attributes state transitions
  if (line.includes(' prompt ')) {
    const p = parsePromptPayload(line);
    if (p && p.id) currentPrompt = p;
  }
  dash_feed(line);                 // same-source data layer (same bugs/data)
  let sigs = [];
  try { sigs = JSON.parse(drain_signals()); } catch {}
  if (sigs.length) pushSignals(sigs);
  render();
};

function setConn() {
  elConnDot.className = 'dot ' + (connected ? 'live' : 'off');
  elConnText.textContent = connected ? 'LIVE' : '已断开 · 重连中';
}

// ── /state poll: bridge/device health for the header ─────────────────────────
function setPill(el, ok, okText, badText) {
  el.textContent = ok ? okText : badText;
  el.className = ok ? 'ok' : 'bad';
}
async function pollState() {
  try {
    const st = await (await fetch('/state')).json();
    setPill(elBridge, st.bridge_reachable, '✓ ' + st.bridge_addr, '✗ ' + st.bridge_addr);
    if (st.serial) {                       // real device: bridge owns COM, mirror taps us
      elLink.textContent = 'serial ' + st.serial; elLink.className = 'ok';
    } else {
      setPill(elLink, st.device_connected, '✓ connected', '✗ no bridge');
    }
    elClients.textContent = st.clients;
    elMode.textContent = (st.serial ? 'real' : 'mock') + ' · ' + (st.gate ? 'gate' : 'observe');
    elMode.className = st.gate ? 'warn' : '';
  } catch {}
}

// ── inject panel ─────────────────────────────────────────────────────────────
async function postJSON(path, body) {
  try {
    const r = await fetch(path, {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    return await r.json();
  } catch (e) { return { error: String(e) }; }
}

function injCtx() {
  return { agent: $('inj-agent').value, session_id: $('inj-session').value || 'web_dev' };
}

const PRESETS = [
  { label: 'prompt', ev: () => ({ type: 'user_prompt_submit', prompt: '从 web 注入的任务' }) },
  { label: 'Read', ev: () => ({ type: 'pre_tool_use', tool_name: 'Read', tool_input: { file_path: 'main/foo.c' } }) },
  { label: 'Bash rm -rf (权限!)', ev: () => ({ type: 'pre_tool_use', tool_name: 'Bash', tool_input: { command: 'rm -rf /tmp/x' } }) },
  { label: 'post Edit', ev: () => ({ type: 'post_tool_use', tool_name: 'Edit', summary: 'Edit main/foo.c', tokens: 120 }) },
  { label: 'tokens +500', ev: () => ({ type: 'tokens', tokens: 500 }) },
  { label: 'stop (continue)', ev: () => ({ type: 'stop', last_assistant_text: '完成了,你来看看。' }) },
  { label: 'stop (2 选项→reply)', ev: () => ({ type: 'stop', dash_state: { summary: '选个方向', options: ['方案 A', '方案 B'] } }) },
];

function buildPresets() {
  const host = $('inj-presets');
  PRESETS.forEach((p, i) => {
    const b = document.createElement('button');
    b.textContent = p.label;
    b.onclick = () => inject({ ...p.ev(), ...injCtx() });
    host.appendChild(b);
  });
}

async function inject(event) {
  const r = $('inj-result');
  r.textContent = `注入 ${event.type}…(权限事件会等待你的决策)`;
  const res = await postJSON('/inject', event);
  r.textContent = `← ${JSON.stringify(res).slice(0, 160)}`;
}

$('inj-send').onclick = () => {
  let ev;
  try { ev = JSON.parse($('inj-free').value); }
  catch (e) { $('inj-result').textContent = 'JSON 解析失败: ' + e; return; }
  inject({ ...injCtx(), ...ev });
};

// ── screen test driver ───────────────────────────────────────────────────────
// Push raw `dash` signals straight to the connected device (real ESP32 or mock)
// to exercise UI combinations. "冻结自动快照" pauses the bridge publisher so a
// hand-pushed combo stays on screen. Uses the device-visible field names
// (agents[].awaiting_kind/awaiting_summary/awaiting_options, prompt.mode, ...).
const A = (kind, status, msg, tokens = 0, extra = {}) =>
  ({ kind, session_id: kind.slice(0, 2) + Math.floor(tokens % 9000 + 1000),
     status, msg, cwd: 'D:\\Code\\demo', tokens, tokens_today: tokens, ...extra });
const SNAP = (agents, totals = {}) => ({
  agents,
  totals: {
    total: agents.length,
    running: agents.filter(a => a.status === 'running').length,
    waiting: agents.filter(a => a.status === 'waiting').length,
    tokens: agents.reduce((s, a) => s + (a.tokens || 0), 0),
    tokens_today: agents.reduce((s, a) => s + (a.tokens_today || 0), 0),
    ...totals,
  },
  // Top-level prompt:null sets prompt_clear → clears prompt_active and leaves
  // the prompt scene. WITHOUT this a plain snapshot can't exit the prompt
  // takeover (only a physical button or this can), so every scene preset
  // includes it — otherwise clicking a `dash prompt` preset traps the device.
  prompt: null,
});
const AW = (kind, ctx, extra = {}) =>
  A('claude-code', 'waiting', '', 0, { awaiting_kind: kind, awaiting_context: ctx,
     awaiting_since: 1700000000, ...extra });

const SCENE_GROUPS = [
  { name: '场景', items: [
    { label: '⟲ 复位 (清 prompt)', cmd: 'snapshot',
      payload: SNAP([A('claude-code', 'running', '> 已复位', 0)]) },
    { label: 'idle / 空', cmd: 'idle' },
    { label: 'dashboard 单 agent', cmd: 'snapshot',
      payload: SNAP([A('claude-code', 'running', '> 修复登录 bug', 1200)]) },
    { label: 'sessions 多 agent', cmd: 'snapshot', payload: SNAP([
      A('claude-code', 'running', '> 重构 bridge', 3400),
      A('codex', 'waiting', '> 写测试', 800),
      A('claude-code', 'idle', '> 已完成', 5000),
    ]) },
    { label: '满槽溢出 (5→丢弃)', cmd: 'snapshot', payload: SNAP(
      [0, 1, 2, 3, 4].map(i => A(i % 2 ? 'codex' : 'claude-code', 'running', `> 任务 ${i}`, i * 700))) },
  ] },
  { name: 'prompt / 决策', items: [
    { label: '权限 (approve/deny)', cmd: 'prompt',
      payload: { id: 'req_demo', tool: 'Bash', hint: '$ rm -rf "/tmp/x"', agent_kind: 'claude-code' } },
    { label: 'quick-reply 2 选项', cmd: 'prompt',
      payload: { id: 'rpl_demo', mode: 'reply', tool: '方案 A', hint: '方案 B' } },
  ] },
  { name: 'awaiting 变体', items: [
    { label: 'continue', cmd: 'snapshot', payload: SNAP([AW('continue', ['轮到你了'])]) },
    { label: 'approve', cmd: 'snapshot', payload: SNAP([AW('approve', ['Bash', '$ rm -rf /tmp/x'])]) },
    { label: 'pick (选项)', cmd: 'snapshot', payload: SNAP([AW('pick', ['选一个方向'],
      { awaiting_summary: '下一步怎么走?', awaiting_options: ['继续实现', '先写测试', '回滚', '问我'] })]) },
    { label: 'type (开放问题)', cmd: 'snapshot', payload: SNAP([AW('type', ['需要你输入参数'],
      { awaiting_summary: '部署到哪个环境?' })]) },
    { label: 'clarify (澄清)', cmd: 'snapshot', payload: SNAP([AW('clarify', ['需求有歧义'],
      { awaiting_summary: '“它”指哪个文件?' })]) },
  ] },
  { name: '其它 / 边界', items: [
    { label: 'tokens 高', cmd: 'snapshot',
      payload: SNAP([A('claude-code', 'running', '> 大任务', 1234567)]) },
    { label: 'push banner', cmd: 'push', payload: { tool: 'Edit', hint: 'main/foo.c' } },
    { label: '主题 lab', cmd: 'config', payload: { theme: 'lab' } },
    { label: '主题 mono', cmd: 'config', payload: { theme: 'mono' } },
    { label: '主题 noir', cmd: 'config', payload: { theme: 'noir' } },
    { label: '长名/unicode', cmd: 'config',
      payload: { device_name: '超长设备名称-test-АБ-😀-overflow-check', owner: '测试者' } },
    { label: '超长 msg', cmd: 'snapshot',
      payload: SNAP([A('claude-code', 'running', '> ' + '很长的消息文本'.repeat(20), 42)]) },
    { label: 'health', cmd: 'health' },
  ] },
];

function buildSceneDriver() {
  const host = $('scene-presets');
  for (const g of SCENE_GROUPS) {
    const wrap = document.createElement('div');
    wrap.className = 'grp';
    const lbl = document.createElement('div');
    lbl.className = 'lbl';
    lbl.textContent = g.name;
    wrap.appendChild(lbl);
    const row = document.createElement('div');
    row.className = 'row';
    for (const it of g.items) {
      const b = document.createElement('button');
      b.textContent = it.label;
      b.onclick = () => sendDash(it.cmd, it.payload ?? null);
      row.appendChild(b);
    }
    wrap.appendChild(row);
    host.appendChild(wrap);
  }
}

async function sendDash(cmd, payload) {
  const r = $('dash-result');
  r.textContent = `→ dash ${cmd}…`;
  const res = await postJSON('/dash', { cmd, payload });
  r.textContent = `← ${JSON.stringify(res).slice(0, 140)}`;
}

$('hold-toggle').onchange = async (e) => {
  const res = await postJSON('/hold', { on: e.target.checked });
  $('dash-result').textContent = e.target.checked
    ? `已冻结自动快照 ${JSON.stringify(res).slice(0, 80)}`
    : `已恢复自动快照 ${JSON.stringify(res).slice(0, 80)}`;
};

$('dash-send').onclick = () => {
  const cmd = $('dash-verb').value;
  const raw = $('dash-payload').value.trim();
  let payload = null;
  if (raw) {
    try { payload = JSON.parse(raw); }
    catch (e) { $('dash-result').textContent = 'JSON 解析失败: ' + e; return; }
  }
  sendDash(cmd, payload);
};

// ── hooks panel ──────────────────────────────────────────────────────────────
async function loadHooks() {
  try { renderHooks(await (await fetch('/hooks')).json()); }
  catch (e) { elHooks.textContent = 'hooks status error: ' + e; }
}
function renderHooks(data) {
  let html = '';
  for (const [kind, s] of Object.entries(data)) {
    const state = !s.installed ? '未装' : (s.enabled ? '✅ 启用' : '⏸ 禁用');
    const color = !s.installed ? 'var(--muted)' : (s.enabled ? 'var(--green)' : 'var(--amber)');
    html += '<div class="row" style="margin:6px 0">' +
      `<span style="width:120px">${esc(s.display_name)}</span>` +
      `<span style="width:64px;color:${color}">${state}</span>` +
      `<button data-a="install" data-k="${kind}">Install</button>` +
      `<button data-a="enable"  data-k="${kind}">Enable</button>` +
      `<button data-a="disable" data-k="${kind}">Disable</button>` +
      `<span class="muted" style="font-size:11px">${esc((s.events || []).join(',') || '-')}</span>` +
      '</div>';
  }
  elHooks.innerHTML = html || '<span class="muted">无 adapter</span>';
  elHooks.querySelectorAll('button').forEach(b => {
    b.onclick = async () => {
      elHooks.querySelectorAll('button').forEach(x => x.disabled = true);
      try { await postJSON('/hooks/' + b.dataset.a, { agent: b.dataset.k }); }
      finally { loadHooks(); }
    };
  });
}

// ── boot ─────────────────────────────────────────────────────────────────────
buildPresets();
buildSceneDriver();
setConn();
render();
loadHooks();
pollState();
setInterval(loadHooks, 4000);
setInterval(pollState, 2000);
