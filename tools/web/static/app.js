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
const elLink = $('h-link'), elClients = $('h-clients'), elAuto = $('h-auto');
const elTotals = $('totals'), elAwaiting = $('awaiting'), elAgents = $('agents');
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

  // totals
  const t = s.totals || {};
  elTotals.innerHTML =
    `<div class="grid2">` +
    `<span class="k">total</span><span>${t.total ?? 0}</span>` +
    `<span class="k">running</span><span class="s-running">${t.running ?? 0}</span>` +
    `<span class="k">waiting</span><span class="s-waiting">${t.waiting ?? 0}</span>` +
    `<span class="k">tokens</span><span>${t.tokens ?? 0}</span>` +
    `<span class="k">tokens_today</span><span>${t.tokens_today ?? 0}</span>` +
    `<span class="k">owner</span><span>${esc(s.owner)}</span>` +
    `</div>`;

  // agents detail
  const slots = s.slots || [];
  if (!slots.length) {
    elAgents.innerHTML = '<span class="muted">无活跃 agent</span>';
  } else {
    let rows = '<table><tr><th>kind</th><th>session</th><th>status</th>' +
      '<th>awaiting</th><th>tok</th><th>msg / cwd</th></tr>';
    for (const a of slots) {
      rows += `<tr><td>${esc(a.kind)}</td><td>${esc(a.session_id)}</td>` +
        `<td class="s-${a.status}">${esc(a.status)}</td>` +
        `<td>${a.awaiting && a.awaiting !== 'none' ? esc(a.awaiting) : '<span class="muted">—</span>'}</td>` +
        `<td>${a.tokens ?? 0}</td>` +
        `<td>${esc(a.msg)}${a.cwd ? `<br><span class="muted">${esc(a.cwd)}</span>` : ''}</td></tr>`;
    }
    elAgents.innerHTML = rows + '</table>';
  }

  // awaiting interaction — only while the device reports prompt active
  const active = s.prompt && s.prompt.active;
  if (!active) currentPrompt = null;
  renderAwaiting(s);

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
      `<div class="awaiting"><b>quick-reply</b> <span class="muted">id=${id}</span>` +
      `<div class="row" style="margin-top:8px">` +
      opts.map((o, i) => `<button class="opt" data-reply="${i}">${i + 1}. ${esc(o)}</button>`).join('') +
      `</div></div>`;
    elAwaiting.querySelectorAll('button[data-reply]').forEach(b => {
      b.onclick = () => sendReply(p.id, +b.dataset.reply);
    });
  } else {
    elAwaiting.innerHTML =
      `<div class="awaiting"><b>permission</b> <span class="muted">id=${id}</span>` +
      `<div class="grid2" style="margin:6px 0">` +
      `<span class="k">tool</span><span>${esc(p.tool)}</span>` +
      `<span class="k">hint</span><span>${esc(p.hint)}</span></div>` +
      `<div class="row">` +
      `<button class="approve" data-dec="once">Approve (once)</button>` +
      `<button class="deny" data-dec="deny">Deny</button></div></div>`;
    elAwaiting.querySelectorAll('button[data-dec]').forEach(b => {
      b.onclick = () => sendDecision(p.id, b.dataset.dec);
    });
  }
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
function pushFrame(line) {
  const verb = line.split(/\s+/)[1] || '?';
  frames.push({ t: now(), verb, line });
  if (frames.length > MAX) frames.shift();
  frameN++;
  elFramesCount.textContent = `${frameN} 帧`;
  elFrames.innerHTML = frames.slice(-120).reverse().map(f =>
    `<div class="ln"><span class="t">${f.t}</span> <span class="v">${esc(f.verb.padEnd(8))}</span> ${esc(f.line.slice(0, 200))}</div>`
  ).join('');
}
function pushSignals(arr) {
  for (const sig of arr) {
    signals.push({ t: now(), sig });
    if (signals.length > MAX) signals.shift();
  }
  if (!signals.length) return;
  elSignals.innerHTML = signals.slice(-120).reverse().map(x =>
    `<div class="ln"><span class="t">${x.t}</span> <span class="sig">${esc(x.sig)}</span></div>`
  ).join('');
}

// ── SSE: feed every real dash line to the WASM data layer ────────────────────
const es = new EventSource('/events');
es.onopen = () => { connected = true; setConn(); };
es.onerror = () => { connected = false; setConn(); };
es.onmessage = (ev) => {
  let line;
  try { line = JSON.parse(ev.data); } catch { line = ev.data; }
  if (typeof line !== 'string') return;
  pushFrame(line);
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
  elConnText.textContent = connected ? 'LIVE' : '已断开(自动重连)';
}

// ── /state poll: bridge/device health for the header ─────────────────────────
async function pollState() {
  try {
    const st = await (await fetch('/state')).json();
    elBridge.textContent = st.bridge_reachable ? `✓ ${st.bridge_addr}` : `✗ ${st.bridge_addr}`;
    elBridge.style.color = st.bridge_reachable ? 'var(--green)' : 'var(--red)';
    elLink.textContent = st.device_connected ? '✓ connected' : '✗ no bridge';
    elLink.style.color = st.device_connected ? 'var(--green)' : 'var(--red)';
    elClients.textContent = st.clients;
    elAuto.textContent = st.auto;
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
});
const AW = (kind, ctx, extra = {}) =>
  A('claude-code', 'waiting', '', 0, { awaiting_kind: kind, awaiting_context: ctx,
     awaiting_since: 1700000000, ...extra });

const SCENE_GROUPS = [
  { name: '场景', items: [
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
    const lbl = document.createElement('div');
    lbl.className = 'muted';
    lbl.style.cssText = 'margin:8px 0 3px;font-size:11px';
    lbl.textContent = g.name;
    host.appendChild(lbl);
    const row = document.createElement('div');
    row.className = 'row';
    for (const it of g.items) {
      const b = document.createElement('button');
      b.textContent = it.label;
      b.onclick = () => sendDash(it.cmd, it.payload ?? null);
      row.appendChild(b);
    }
    host.appendChild(row);
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
