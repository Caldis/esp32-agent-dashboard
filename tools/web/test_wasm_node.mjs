// node ESM smoke: 加载 emcc 出的 wasm 数据层,验证 JS 运行时下行为一致。
// 运行:bash tools/web/wasm/build_wasm.sh && node tools/web/test_wasm_node.mjs
import assert from 'node:assert';
import DashDataLayer from './static/dash_datalayer.mjs';

const M = await DashDataLayer();
const dash_init  = M.cwrap('dash_init', null, []);
const dash_feed  = M.cwrap('dash_feed_line', 'number', ['string']);
const state_json = M.cwrap('state_json', 'string', []);

dash_init();
let s = JSON.parse(state_json());
assert.strictEqual(s.device_name, 'DASHBOARD', 'default device_name');
assert.strictEqual(s.totals.total, 0, 'empty totals');
assert.deepStrictEqual(s.slots, [], 'empty slots');

const snap = '{"agents":[{"kind":"codex","session_id":"cx1","status":"running",'
           + '"msg":"go","tokens":42}],"totals":{"total":1,"running":1,"waiting":0}}';
const rc = dash_feed('dash snapshot "' + snap + '"');
assert.strictEqual(rc, 0, 'feed rc');
s = JSON.parse(state_json());
assert.strictEqual(s.totals.total, 1, 'one agent');
assert.strictEqual(s.slots.length, 1, 'one slot');
assert.strictEqual(s.slots[0].kind, 'codex', 'kind');
assert.strictEqual(s.slots[0].tokens, 42, 'tokens');

console.log('wasm node smoke: ALL PASS');
