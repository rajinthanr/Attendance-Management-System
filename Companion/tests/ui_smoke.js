/*
 * Drives the real page in headless Chrome against the helper running on demo
 * data, and checks what a person would see and what lands in the device's
 * files. Needs Node 22+, Python and Chrome (or Edge); set CHROME to override.
 *
 *   node tests/ui_smoke.js
 */
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');

const ROOT = path.resolve(__dirname, '..');
const APP_PORT = 8798;
const DEBUG_PORT = 9333;

let run = 0;
let failed = 0;
function check(cond, name, detail) {
  run++;
  if (!cond) { failed++; console.log('  FAIL ' + name + (detail !== undefined ? '\n       ' + detail : '')); }
}

function findChrome() {
  const c = [process.env.CHROME,
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'].filter(Boolean);
  return c.find((p) => fs.existsSync(p));
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function getJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let b = '';
      res.on('data', (d) => (b += d));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}
function getText(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => { let b = ''; res.on('data', (d) => (b += d)); res.on('end', () => resolve(b)); }).on('error', reject);
  });
}

async function until(fn, ms, what) {
  const end = Date.now() + ms;
  for (;;) {
    try { const v = await fn(); if (v) { return v; } } catch (e) { /* retry */ }
    if (Date.now() > end) { throw new Error('timed out waiting for ' + what); }
    await sleep(100);
  }
}

(async function main() {
  const chromePath = findChrome();
  if (!chromePath) { console.log('No Chrome or Edge found; skipping the UI test.'); process.exit(0); }

  // ---- the helper, on demo data
  const server = spawn(process.platform === 'win32' ? 'python' : 'python3',
    ['attendance_app.py', '--demo', '--no-browser', '--port', String(APP_PORT)], { cwd: ROOT });
  let serverOut = '';
  server.stdout.on('data', (d) => (serverOut += d));
  server.stderr.on('data', (d) => (serverOut += d));
  await until(() => getJson('http://127.0.0.1:' + APP_PORT + '/api/ping'), 10000, 'the helper to start');
  await until(() => /Demo data in (.+?) and (.+?) \(/.test(serverOut), 5000, 'the demo folder');
  const demoDir = /Demo data in (.+?) and (.+?) \(/.exec(serverOut)[2];

  // ---- headless Chrome
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'att-chrome-'));
  const chrome = spawn(chromePath, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--remote-debugging-port=' + DEBUG_PORT, '--user-data-dir=' + profile, '--window-size=1280,900', 'about:blank']);
  let ws, nextId = 1;
  const pending = {};
  const errors = [];
  const cleanup = () => { try { chrome.kill(); } catch (e) { /* gone */ } try { server.kill(); } catch (e) { /* gone */ } };
  process.on('exit', cleanup);

  try {
    const targets = await until(async () => {
      const t = await getJson('http://127.0.0.1:' + DEBUG_PORT + '/json');
      return t.find((x) => x.type === 'page');
    }, 15000, 'Chrome to start');
    ws = new WebSocket(targets.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    ws.onmessage = (m) => {
      const msg = JSON.parse(m.data);
      if (msg.id && pending[msg.id]) { pending[msg.id](msg); delete pending[msg.id]; return; }
      if (msg.method === 'Runtime.exceptionThrown') { errors.push(msg.params.exceptionDetails.exception ? msg.params.exceptionDetails.exception.description : msg.params.exceptionDetails.text); }
      if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') { errors.push(msg.params.args.map((a) => a.value || a.description).join(' ')); }
    };
    const send = (method, params) => new Promise((res) => { const id = nextId++; pending[id] = res; ws.send(JSON.stringify({ id, method, params: params || {} })); });
    const ev = async (expr) => {
      const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
      if (r.result.exceptionDetails) { throw new Error(r.result.exceptionDetails.exception.description); }
      return r.result.result.value;
    };
    const click = (sel) => ev('(function(){var e=document.querySelector(' + JSON.stringify(sel) + ');if(!e)throw new Error("no element ' + sel.replace(/"/g, '') + '");e.click();return true})()');
    const type = (sel, v) => ev('(function(){var e=document.querySelector(' + JSON.stringify(sel) + ');if(!e)throw new Error("no element");e.value=' + JSON.stringify(v) + ';e.dispatchEvent(new Event("input",{bubbles:true}));return true})()');
    const text = (sel) => ev('(function(){var e=document.querySelector(' + JSON.stringify(sel) + ');return e?e.innerText:null})()');
    const count = (sel) => ev('document.querySelectorAll(' + JSON.stringify(sel) + ').length');
    const tab = (t) => click('#tabs button[data-tab="' + t + '"]');
    const shot = async (name) => {
      const r = await send('Page.captureScreenshot', { format: 'png' });
      if (process.env.SHOTS) { fs.writeFileSync(path.join(process.env.SHOTS, name + '.png'), Buffer.from(r.result.data, 'base64')); }
    };
    const base = 'http://127.0.0.1:' + APP_PORT;
    const api = (p) => getJson(base + p);
    const setSel = (sel, v) => ev('(function(){var e=document.querySelector(' + JSON.stringify(sel) + ');e.value=' + JSON.stringify(v) + ';e.dispatchEvent(new Event("change",{bubbles:true}));return true})()');
    const settingsFile = () => fs.readFileSync(path.join(demoDir, 'SETTINGS.CSV'), 'utf8');

    await send('Runtime.enable');
    await send('Page.enable');
    await send('Page.navigate', { url: base + '/' });
    await until(() => ev('window.__att && window.__att.S.state && window.__att.S.students.length > 0'), 15000, 'the page to load');

    /* ---- header and lecture tab --------------------------------------- */
    check(/Demo device/.test(await text('#device-pill')), 'pill says a device is connected', await text('#device-pill'));
    check(/Start a lecture/.test(await text('#tab-lecture')), 'the lecture tab is first');
    check(/Lecture in progress/.test(await text('#tab-lecture')), 'the running lecture is shown');
    check(await ev('document.querySelector("#f-title").value') === 'Circuits Lecture 5', 'next lecture name is suggested', await ev('document.querySelector("#f-title").value'));
    await shot('1-lecture');
    await type('#f-module', 'MA1010');
    await type('#f-title', 'Calculus Lecture 3');
    await click('[data-action="start"]');
    await until(async () => /Sent to the device/.test(await text('#banners')), 8000, 'the sent banner');
    const sf = settingsFile();
    check(/#MODULE,MA1010\r?\n/.test(sf) && /#LECTURE,Calculus Lecture 3\r?\n/.test(sf), 'names written to SETTINGS.CSV', sf);
    check(/#NEWSESSION,1/.test(sf) && /#TIME,\d{4}-/.test(sf), 'new session and clock are sent');
    check((await api('/api/state')).current_lecture.title === 'Calculus Lecture 3', 'the lecture is recorded in the database');

    /* ---- attendance --------------------------------------------------- */
    await tab('attendance');
    await until(async () => (await count('#session-list .session')) > 5, 5000, 'lecture list');
    await click('#session-list .session[data-id="1"], #session-list .session:nth-child(4)');
    await until(async () => /present/.test(await text('#session-detail')), 5000, 'lecture detail');
    check(await count('#table-slot tbody tr') > 3, 'present table has rows');
    const rowsAll = await count('#table-slot tbody tr');
    await type('#search', 'perera');
    const filt = await count('#table-slot tbody tr');
    check(filt > 0 && filt < rowsAll, 'search narrows (' + filt + ' of ' + rowsAll + ')');
    await type('#search', '');
    await click('[data-action="view"][data-v="absent"]');
    check(await count('#table-slot .tag.bad') > 0, 'absent view lists absentees');
    check(/Download PDF/.test(await text('#session-detail')) && /Download CSV/.test(await text('#session-detail')), 'PDF and CSV buttons are there');
    await click('#session-list .session[data-id="taps"]');
    await until(async () => /Every tap/.test(await text('#session-detail')), 3000, 'taps view');
    await until(async () => (await count('#table-slot tbody tr')) > 40, 5000, 'tap rows').catch(() => {});
    check(await count('#table-slot tbody tr') > 40, 'every tap can be listed', await count('#table-slot tbody tr'));
    check(/Unregistered/.test(await text('#table-slot')), 'unknown cards are labelled');
    await shot('2-attendance');

    /* ---- students: register a new card -------------------------------- */
    await tab('students');
    await until(async () => (await count('#st-table tbody tr')) > 20, 5000, 'student table');
    check(/New cards seen by the device \(2\)/.test(await text('#tab-students')), 'two new cards are offered', (await text('#tab-students')).slice(0, 120));
    await type('#st-q', 'perera');
    const sr = await count('#st-table tbody tr');
    check(sr > 0 && sr < 30, 'student search narrows', sr);
    await type('#st-q', '');
    await shot('3-students');
    await click('.newcards [data-action="register"]');
    await until(() => ev('document.querySelector("#dlg").open'), 2000, 'the dialog');
    check(/0000424242|0000535353/.test(await ev('document.querySelector("#sd-card").value')), 'the card number is filled in');
    await type('#sd-name', 'Test Student');
    await type('#sd-no', 'EN/22/999');
    await type('#sd-dept', 'Electrical Engineering');
    await ev('(function(){var c=document.querySelector("#sd-mods input[data-mod=MA1010]");c.checked=true;return true})()');
    await click('[data-action="save-student"]');
    await until(async () => !(await ev('document.querySelector("#dlg").open')), 3000, 'dialog to close');
    const saved = (await api('/api/students')).students.find((s) => s.name === 'Test Student');
    check(saved && saved.student_no === 'EN/22/999' && saved.modules.indexOf('MA1010') >= 0, 'the student was saved to the database', JSON.stringify(saved));
    await until(async () => /New cards seen by the device \(1\)/.test(await text('#tab-students')), 3000, 'new cards list to shrink');
    await until(async () => /does not have your latest student cards/.test(await text('#banners')), 4000, 'the out-of-date banner');
    check(/knows 30 and the list has 31/.test(await text('#banners')), 'the banner says how far behind the device is', await text('#banners'));
    check(/knows 30 of 31 cards/.test(await text('#tab-students')), 'the students tab says so too');
    await click('#banners [data-action="send-cards"]');
    await until(() => ev('1').then(() => /#CARDS,31/.test(settingsFile())), 4000, 'the card list on the device');
    check(!/Test Student/.test(settingsFile()), 'no names are written to the device');
    check(settingsFile().indexOf('0000424242') >= 0 || settingsFile().indexOf('0000535353') >= 0, 'the registered card number is in the list');
    check(true, 'the registered card leaves the new-cards list');

    // validation
    await click('[data-action="add-student"]');
    await type('#sd-card', 'abc');
    check(/digits only/.test(await text('#sd-card-err')), 'a bad card number is flagged');
    await click('[data-action="save-student"]');
    await until(async () => (await text('#sd-err')).length > 0, 3000, 'an error message');
    check(await ev('document.querySelector("#dlg").open'), 'the dialog stays open on error');
    await click('[data-action="close-dialog"]');

    // import
    await click('[data-action="import-students"]');
    await ev('document.querySelector("#im-text").value="card_id,name,student_no,department,modules\\n777001,Imported One,X1,Physics,MA1010;PH1010\\nbad,Nope,,,"');
    await click('[data-action="run-import"]');
    await until(async () => /1 student added/.test(await text('#im-result')), 4000, 'import result');
    check(/Line 3/.test(await text('#im-result')), 'a bad import row is reported by line', await text('#im-result'));
    await click('[data-action="close-dialog"]');
    check((await api('/api/modules')).modules.some((m) => m.code === 'PH1010'), 'a new module in the import is created');

    // edit
    await click('#st-table [data-action="edit-student"]');
    check(/Edit student/.test(await text('#dlg-body')) && await ev('document.querySelector("#sd-card").readOnly'), 'edit opens with the card locked');
    await click('[data-action="close-dialog"]');

    /* ---- modules ------------------------------------------------------ */
    await tab('modules');
    check(await count('#tab-modules tbody tr') >= 4, 'modules are listed', await count('#tab-modules tbody tr'));
    await click('[data-action="enrol-module"][data-code="PH1010"]');
    await until(() => ev('document.querySelector("#dlg").open'), 2000, 'enrol dialog');
    check(await count('#en-list input:checked') === 1, 'the imported student is enrolled in PH1010');
    await type('#en-q', 'perera');
    await click('[data-action="en-all"]');
    await click('[data-action="save-enrol"]');
    await until(async () => !(await ev('document.querySelector("#dlg").open')), 3000, 'enrol to save');
    check((await api('/api/modules')).modules.find((m) => m.code === 'PH1010').students > 1, 'enrolment was saved');

    /* ---- reports ------------------------------------------------------ */
    await tab('reports');
    await until(async () => (await count('#rep-body tbody tr')) > 5, 5000, 'module report');
    check(await count('#rep-body thead th.c') >= 3, 'the report has a column per lecture');
    check(/Download PDF/.test(await text('#rep-body')), 'report PDF button is there');
    await click('[data-action="rep-mode"][data-m="student"]');
    await setSel('#r-card', String(saved.card_id));
    await until(async () => /Test Student/.test(await text('#rep-body')), 4000, 'student report');
    check(true, 'a student report opens');
    await shot('4-reports');

    /* ---- the device goes away and comes back --------------------------- */
    fs.renameSync(path.join(demoDir, 'STATUS.TXT'), path.join(demoDir, 'STATUS.hide'));
    await until(async () => /Waiting for the device/.test(await text('#device-pill')), 8000, 'unplugged state');
    await tab('attendance');
    check(await count('#session-list .session') > 5, 'data stays on screen after unplugging');
    await tab('lecture');
    check(await ev('document.querySelector("[data-action=start]").disabled'), 'starting a lecture needs the device');
    fs.renameSync(path.join(demoDir, 'STATUS.hide'), path.join(demoDir, 'STATUS.TXT'));
    await until(async () => /Demo device/.test(await text('#device-pill')), 8000, 'plugged back in');

    /* ---- the device confirms, then refuses ---------------------------- */
    const st = fs.readFileSync(path.join(demoDir, 'STATUS.TXT'), 'utf8');
    await ev('window.__att.S.sent = {module: "EN2090", title: "Circuits Lecture 4"}');
    await until(async () => /Lecture started on the device/.test(await text('#banners')), 8000, 'applied banner');
    check(true, 'the app confirms when the device shows the lecture');
    fs.writeFileSync(path.join(demoDir, 'STATUS.TXT'), st.replace('SETTINGS.CSV : unchanged', 'SETTINGS.CSV : ERROR, bad date on line 4. Nothing was saved'));
    await until(async () => /refused/.test(await text('#banners')), 8000, 'refusal banner');
    check(/line 4/.test(await text('#banners')), 'the refusal names the line');

    check(errors.length === 0, 'no errors in the browser console', errors.join('\n'));
  } catch (e) {
    failed++;
    console.log('  FAIL ' + e.message);
  } finally {
    cleanup();
    await sleep(300);
    try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) { /* in use: fine */ }
  }
  console.log('\n' + run + ' checks, ' + failed + ' failures');
  process.exit(failed ? 1 : 0);
})();
