// 在浏览器里加载固件同源数据层(wasm),feed 一条样本 snapshot,渲染 state_json。
import DashDataLayer from './dash_datalayer.mjs';

const out = document.getElementById('out');
try {
  const M = await DashDataLayer();
  const dash_init     = M.cwrap('dash_init', null, []);
  const dash_feed     = M.cwrap('dash_feed_line', 'number', ['string']);
  const state_json    = M.cwrap('state_json', 'string', []);
  const current_scene = M.cwrap('current_scene', 'string', []);

  dash_init();
  const snap = '{"agents":[{"kind":"claude-code","session_id":"cc_demo",'
             + '"status":"running","msg":"editing main.c","tokens":1234}],'
             + '"totals":{"total":1,"running":1,"waiting":0}}';
  const rc = dash_feed('dash snapshot "' + snap + '"');
  const state = JSON.parse(state_json());
  out.textContent = 'feed rc=' + rc + '\nscene=' + current_scene() + '\n'
                  + JSON.stringify(state, null, 2);
} catch (e) {
  out.textContent = 'ERROR: ' + (e && e.message ? e.message : e)
                  + '\n\n先运行 tools/web/wasm/build_wasm.sh 生成 dash_datalayer.mjs/.wasm';
}
