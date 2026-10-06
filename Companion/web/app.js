/*
 * app.js: the screens. The database, the matching of taps to people and
 * lectures, and every export live in the helper (attendance_app.py); this
 * file shows them and asks the helper to change things.
 */
(function () {
  'use strict';

  var POLL_MS = 2000;

  function $(sel, root) { return (root || document).querySelector(sel); }
  function $$(sel, root) { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }
  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : (many || one + 's')); }
  function card10(n) { var s = String(n); while (s.length < 10) { s = '0' + s; } return s; }
  function lsGet(k, d) { try { var v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } }
  function lsSet(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* private mode: fine */ } }
  function q(params) {
    var out = [];
    Object.keys(params).forEach(function (k) { if (params[k] !== '' && params[k] != null) { out.push(encodeURIComponent(k) + '=' + encodeURIComponent(params[k])); } });
    return out.length ? '?' + out.join('&') : '';
  }

  /* ------------------------------------------------------------ state */
  var S = {
    tab: 'lecture',
    online: false,
    state: null,                 // the helper's view of the device, the database counts, the current lecture
    students: [], departments: [], modules: [], lectures: [], unassigned: [], cards: [],
    sel: null,                   // attendance: lecture id, 'taps', or 'day:YYYY-MM-DD'
    detail: null, taps: [], view: 'present', search: '',
    filter: { q: '', module: '', department: '' },
    repMode: 'module', rep: { module: '', from: '', to: '', card: '' }, repData: null,
    form: { module: '', title: '', sync: true, touched: false },
    sent: null,                  // the lecture just sent to the device, waiting for the cable to come out
    waiting: null,               // {known: [...]} while the student form waits for a card to be read
    dismissed: {},
    lastSeq: 0
  };

  /* -------------------------------------------------------------- api */
  function api(method, path, body) {
    var opts = { method: method, headers: { 'X-Attendance': '1' } };
    if (body !== undefined) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
    return fetch(path, opts).then(function (r) {
      return r.text().then(function (t) {
        var j = null;
        try { j = t ? JSON.parse(t) : {}; } catch (e) { /* not JSON */ }
        if (!r.ok) { throw new Error((j && j.error) || ('Something went wrong (' + r.status + ')')); }
        return j;
      });
    });
  }

  function fail(e) { toast(e && e.message ? e.message : 'Something went wrong'); }

  function toast(msg) {
    var t = $('#toast');
    t.textContent = msg;
    t.hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(function () { t.hidden = true; }, 4200);
  }

  /** Fetch a file from the helper and hand it to the browser as a download. */
  function download(path) {
    fetch(path, { headers: { 'X-Attendance': '1' } }).then(function (r) {
      if (!r.ok) { return r.json().then(function (j) { throw new Error(j.error || 'Could not make that file'); }); }
      var cd = r.headers.get('Content-Disposition') || '';
      var m = /filename="([^"]+)"/.exec(cd);
      return r.blob().then(function (b) { return { blob: b, name: m ? m[1] : 'download' }; });
    }).then(function (f) {
      var a = document.createElement('a');
      a.href = URL.createObjectURL(f.blob);
      a.download = f.name;
      document.body.appendChild(a);
      a.click();
      setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 500);
    }).catch(fail);
  }

  function printPage(path) { window.open(path + (path.indexOf('?') >= 0 ? '&' : '?') + 'print=1', '_blank'); }

  /* ----------------------------------------------------------- dialog */
  function openDialog(html) {
    var d = $('#dlg');
    $('#dlg-body').innerHTML = html;
    if (!d.open) { d.showModal(); }
    var first = $('#dlg-body input:not([type=hidden]):not([readonly]), #dlg-body textarea');
    if (first) { first.focus(); }
  }
  function closeDialog() { var d = $('#dlg'); if (d.open) { d.close(); } S.waiting = null; }
  $('#dlg').addEventListener('cancel', function () { S.waiting = null; });

  /* ------------------------------------------------------------- data */
  function loadAll() {
    return Promise.all([
      api('GET', '/api/students').then(function (r) { S.students = r.students; S.departments = r.departments; }),
      api('GET', '/api/modules').then(function (r) { S.modules = r.modules; }),
      api('GET', '/api/lectures').then(function (r) { S.lectures = r.lectures; S.unassigned = r.unassigned; }),
      api('GET', '/api/cards/unregistered').then(function (r) { S.cards = r.cards; })
    ]).then(function () { return true; }, function (e) { fail(e); return false; });
  }

  function poll() {
    api('GET', '/api/state').then(function (st) {
      var first = !S.state;
      var prev = S.state;
      S.online = true;
      S.state = st;
      var changed = first || (prev && (prev.connected !== st.connected || prev.sync.seq !== st.sync.seq));
      if (prev && st.sync.seq !== prev.sync.seq && st.sync.new > 0) {
        toast(plural(st.sync.new, 'new tap') + ' read from the device');
      }
      if (st.connected) { checkSent(); }
      renderChrome();
      if (changed) { loadAll().then(function () { checkWaiting(); refreshSelected().then(render); }); }
    }).catch(function () {
      var was = S.online;
      S.online = false;
      renderChrome();
      if (was) { render(); }
    });
  }

  /** The device took the lecture we sent once its status shows those names. */
  function checkSent() {
    var d = S.state && S.state.device;
    if (!S.sent || !d) { return; }
    if (d.has_lecture && d.module === S.sent.module && d.lecture === S.sent.title && !d.pending) {
      S.sent.done = true;
    }
  }

  function refreshSelected() {
    if (typeof S.sel === 'number') {
      return api('GET', '/api/lectures/' + S.sel).then(function (d) { S.detail = d; }, function () { S.sel = null; S.detail = null; });
    }
    if (S.sel === 'taps') {
      return api('GET', '/api/taps?limit=500').then(function (r) { S.taps = r.taps; }, function () { /* keep */ });
    }
    return Promise.resolve();
  }

  /** The student form is waiting for a card: fill it in when a new, unregistered one is read. */
  function checkWaiting() {
    if (!S.waiting) { return; }
    var fresh = S.cards.filter(function (c) { return S.waiting.known.indexOf(c.card_id) < 0; });
    var input = $('#sd-card');
    if (fresh.length && input) {
      input.value = card10(fresh[0].card_id);
      input.dispatchEvent(new Event('input', { bubbles: true }));
      S.waiting = null;
      var w = $('#sd-waiting');
      if (w) { w.hidden = true; }
      toast('Card ' + card10(fresh[0].card_id) + ' read from the device');
    }
  }

  /* ----------------------------------------------------------- chrome */
  function renderChrome() {
    var pill = $('#device-pill');
    var st = S.state;
    var cls, html;
    if (S.online && st && st.connected) {
      var d = st.device || {};
      var bits = [];
      if (st.path) { bits.push(esc(st.path)); }
      bits.push(plural(d.records, 'tap') + ' on the device');
      if (st.drift_seconds != null && Math.abs(st.drift_seconds) > 120) {
        bits.push('clock ' + (st.drift_seconds > 0 ? 'ahead' : 'behind') + ' ' + Math.round(Math.abs(st.drift_seconds) / 60) + ' min');
      }
      cls = 'pill pill-ok';
      html = '<span class="dot"></span><span><b>' + (st.demo ? 'Demo device' : 'Device connected') + '</b> <small>' + bits.join(' · ') + '</small></span>';
    } else if (S.online) {
      cls = 'pill pill-wait';
      html = '<span class="dot"></span><span><b>Waiting for the device</b> <small>plug it in with the USB-C cable</small></span>';
    } else {
      cls = 'pill pill-off';
      html = '<span class="dot"></span><span><b>Not connected to the app</b> <small>start it with attendance_app.py</small></span>';
    }
    pill.className = cls;
    if (pill.innerHTML !== html) { pill.innerHTML = html; }
    $('#quit-btn').hidden = !S.online;
    $('#clock-btn').hidden = !(S.online && st && st.connected);
    var dl = $('#st-devline');
    if (dl) {
      var dlh = deviceCardsLine();
      if (dl.innerHTML !== dlh) { dl.innerHTML = dlh; }
      $('#st-send').disabled = !(S.online && st && st.connected);
    }
    var c = st && st.counts;
    $('#foot-note').textContent = !c ? '' : (st.demo ? 'Demo data: nothing here is real. ' : '') +
      plural(c.students, 'student') + ' · ' + plural(c.lectures, 'lecture') + ' · ' + plural(c.taps, 'tap') + ' in the database';
    renderBanners();
  }

  function banner(key, kind, title, text) {
    if (S.dismissed[key]) { return ''; }
    return '<div class="banner ' + kind + '" role="alert"><div><b>' + esc(title) + '</b>' + esc(text || '') +
           '</div><button class="link x" data-action="dismiss" data-key="' + esc(key) + '" aria-label="Dismiss">✕</button></div>';
  }

  function renderBanners() {
    var h = '';
    var st = S.state;
    if (S.online && st && st.connected && st.device) {
      var d = st.device;
      if (d.error) {
        h += banner('err:' + d.error, 'bad', 'The device refused the last settings file', ' ' + d.error.replace(/^ERROR,?\s*/, ''));
      }
      if (d.pending) { h += banner('pending', 'info', 'Changes are waiting on the device.', ' Eject the drive and unplug the cable to apply them.'); }
      var cs = st.cards;
      if (cs && cs.too_many) {
        h += banner('cardsmany', 'bad', 'There are more students than the device can hold.',
          ' It holds 1000 cards and the list has ' + cs.database + '. Delete the students who have left, then send the cards.');
      } else if (cs && cs.in_sync === false && !d.pending) {
        h += '<div class="banner warn" role="alert"><div><b>The device does not have your latest student cards.</b> ' +
          'It knows ' + cs.device + ' and the list has ' + cs.database + '. Until it is updated, new cards show red when tapped.</div>' +
          '<button class="btn sm primary" data-action="send-cards">Send to device</button></div>';
      }
      if (st.drift_seconds != null && Math.abs(st.drift_seconds) > 120 && S.tab !== 'lecture') {
        h += banner('drift', 'warn', 'The device clock is ' + Math.round(Math.abs(st.drift_seconds) / 60) + ' minutes ' + (st.drift_seconds > 0 ? 'ahead of' : 'behind') + ' this computer.',
          ' Starting a lecture sets it.');
      }
    }
    if (S.sent && S.sent.done) {
      h += banner('applied:' + S.sent.title, 'ok', 'Lecture started on the device ✓',
        ' ' + S.sent.module + ' · ' + S.sent.title + '. Students can tap their cards now.');
    } else if (S.sent) {
      h += banner('await:' + S.sent.title, 'info', 'Sent to the device.',
        ' Now eject the drive and unplug the cable. The device checks the file as the cable comes out: green light and two buzzes means started, red light and three buzzes means it was refused.');
    }
    if (!S.online) {
      h += banner('offline', 'info', 'The app is not running.', ' Close this page and start attendance_app.py (or double-click start.bat).');
    }
    var el = $('#banners');
    if (el.innerHTML !== h) { el.innerHTML = h; }
  }

  /* ---------------------------------------------------------- render */
  function render() {
    renderChrome();
    $$('#tabs button').forEach(function (b) { b.setAttribute('aria-selected', b.dataset.tab === S.tab ? 'true' : 'false'); });
    ['lecture', 'attendance', 'students', 'modules', 'reports'].forEach(function (t) { $('#tab-' + t).hidden = (t !== S.tab); });
    var a = document.activeElement;
    if (a && a.closest && a.closest('#tab-' + S.tab) && /^(INPUT|TEXTAREA|SELECT)$/.test(a.tagName) && render.keep) { return; }
    ({ lecture: renderLecture, attendance: renderAttendance, students: renderStudents, modules: renderModules, reports: renderReports })[S.tab]();
  }

  function empty(title, text) { return '<div class="empty"><b>' + esc(title) + '</b>' + esc(text || '') + '</div>'; }

  function moduleOptions(selected) {
    return S.modules.map(function (m) { return '<option value="' + esc(m.code) + '"' + (m.code === selected ? ' selected' : '') + '>' + esc(m.code + (m.title ? ' — ' + m.title : '')) + '</option>'; }).join('');
  }

  /* ---------------------------------------------------- tab: lecture */
  function suggestTitle(module) {
    var last = '';
    for (var i = 0; i < S.lectures.length; i++) {
      if (!module || S.lectures[i].module_code === module) { last = S.lectures[i].title; break; }
    }
    if (S.sent && S.sent.module === module) { last = S.sent.title; }
    var m = /^(.*?)(\d+)\s*$/.exec(last);
    if (m) { return m[1] + (parseInt(m[2], 10) + 1); }
    return last ? last + ' 2' : 'Lecture 1';
  }

  function renderLecture() {
    var el = $('#tab-lecture');
    var st = S.state || {};
    var cur = st.current_lecture;
    var f = S.form;
    if (!f.touched) {
      f.module = (S.sent && S.sent.module) || (cur && cur.module_code) || lsGet('lastModule', '') || (S.modules[0] && S.modules[0].code) || '';
      f.title = suggestTitle(f.module);
    }
    var info = S.lectures.filter(function (l) { return cur && l.id === cur.id; })[0];
    var live = '';
    if (cur) {
      var c = info ? info.counts : { present_enrolled: 0, enrolled: 0, unregistered: 0, present_other: 0 };
      live = '<div class="card stack"><div class="row between"><div><span class="live">Lecture in progress</span>' +
        '<h2 style="margin-top:.35rem">' + esc(cur.title) + '</h2>' +
        '<div class="muted">' + esc(cur.module_code) + ' · started ' + esc(cur.start_text.slice(11, 16)) + ' on ' + esc(cur.date) + '</div></div>' +
        '<div class="right"><div class="big-num">' + c.present_enrolled + '<span class="muted" style="font-size:1.2rem"> / ' + c.enrolled + '</span></div><div class="muted">present so far</div></div></div>' +
        '<div class="row"><button class="btn" data-action="open-lecture" data-id="' + cur.id + '">See who is here</button>' +
        '<button class="btn" data-action="end-lecture" data-id="' + cur.id + '">End this lecture</button>' +
        '<span class="muted small">Counts update each time the device is plugged in.</span></div></div>';
    }
    var why = '';
    if (!S.online) { why = 'Start the app to continue.'; }
    else if (!st.connected) { why = 'Plug in the device to send the lecture to it.'; }
    else if (!f.module.trim()) { why = 'Choose or type a module.'; }
    else if (!f.title.trim()) { why = 'Give the lecture a name.'; }
    var canSend = !why && !S.busy;
    var dev = st.device || {};
    var drift = st.drift_seconds;
    var driftTxt = '';
    if (st.connected && drift != null) {
      driftTxt = Math.abs(drift) <= 120 ? 'The device clock is right.' : 'The device clock is ' + Math.round(Math.abs(drift) / 60) + ' minutes ' + (drift > 0 ? 'ahead' : 'behind') + '.';
    }
    var mods = S.modules.map(function (m) { return '<option value="' + esc(m.code) + '">' + esc(m.title) + '</option>'; }).join('');
    el.innerHTML = live +
      '<div class="card stack"><div><h2>Start a lecture</h2>' +
      '<p class="muted">Tell the device which lecture this is. Cards tapped afterwards belong to it, and each card is counted once per lecture.</p></div>' +
      '<div class="grid two">' +
        '<label class="field"><span>Module</span><input type="text" id="f-module" list="modules" maxlength="24" autocomplete="off" placeholder="e.g. EN2090" value="' + esc(f.module) + '"><datalist id="modules">' + mods + '</datalist>' +
          '<div class="hint">' + (S.modules.length ? 'Pick one, or type a new code.' : 'Type the module code, for example EN2090.') + '</div></label>' +
        '<label class="field"><span>Lecture</span><input type="text" id="f-title" maxlength="32" autocomplete="off" placeholder="e.g. Lecture 4" value="' + esc(f.title) + '">' +
          '<div class="hint">Up to 32 letters. <button class="link" type="button" data-action="suggest">Use “' + esc(suggestTitle(f.module)) + '”</button></div></label>' +
      '</div>' +
      '<label class="check"><input type="checkbox" id="f-sync"' + (f.sync ? ' checked' : '') + '><span><b>Set the device clock to this computer’s time</b><br>' +
        '<span class="hint">' + esc(st.now || '') + (driftTxt ? ' · ' + esc(driftTxt) : '') + '</span></span></label>' +
      '<div class="row"><button class="btn primary big" data-action="start" ' + (canSend ? '' : 'disabled') + '>Send to device</button>' +
        '<span class="muted">' + esc(why) + '</span></div>' +
      '<div class="small muted">Device not with you? <button class="link" data-action="record-only" ' + (S.online && f.module.trim() && f.title.trim() ? '' : 'disabled') + '>Record the lecture here only</button>' +
      ' (it will not be sent to the device).</div></div>' +
      '<div class="card"><h3>How a lecture works</h3><ol class="steps">' +
        '<li><b>Send to device</b> puts the module and lecture name, and the card numbers of your students, on the drive.</li>' +
        '<li><b>Eject and unplug</b> the cable. The device checks the file: <span class="led green"></span> green light and two buzzes = started, <span class="led red"></span> red light and three buzzes = refused.</li>' +
        '<li><b>Students tap their cards.</b> The device stores only the card number and the time. <span class="led green"></span> Green and a short buzz: a registered card. <span class="led red"></span> Red and a long buzz: a card that is not in your list (it is still recorded, so you can register it). A second tap in the same lecture gets a double buzz and is ignored.</li>' +
        '<li><b>Plug the device back in.</b> This page reads the taps, matches the cards to your student list, and shows who came.</li>' +
      '</ol></div>';
  }

  function startLecture() {
    var f = S.form;
    S.busy = true;
    renderLecture();
    api('POST', '/api/lectures/start', { module: f.module, title: f.title, sync_clock: f.sync }).then(function (r) {
      S.busy = false;
      S.sent = { module: r.lecture.module_code, title: r.lecture.title, done: false };
      S.dismissed = {};
      lsSet('lastModule', r.lecture.module_code);
      f.touched = false;
      toast('Sent. Now eject and unplug the cable.');
      return loadAll();
    }).then(function () { render(); poll(); }).catch(function (e) { S.busy = false; fail(e); render(); });
  }

  /* ------------------------------------------------ tab: attendance */
  function lectureItem(l) {
    var c = l.counts;
    var cur = (S.sel === l.id) ? ' aria-current="true"' : '';
    return '<button class="session" data-action="pick" data-id="' + l.id + '"' + cur + '><b>' + esc(l.title) + (l.running ? ' <span class="chip">running</span>' : '') + '</b>' +
      '<span>' + esc(l.module_code) + ' · ' + esc(l.date) + ' · ' + esc(l.start_text.slice(11, 16)) + '<br>' +
      c.present_enrolled + ' of ' + c.enrolled + ' present (' + c.percent + '%)' + (c.unregistered ? ' · ' + c.unregistered + ' unregistered' : '') + '</span></button>';
  }

  function renderAttendance() {
    var el = $('#tab-attendance');
    var list = '<button class="session" data-action="pick" data-id="taps"' + (S.sel === 'taps' ? ' aria-current="true"' : '') + '><b>Every tap</b><span>' +
      plural((S.state && S.state.counts.taps) || 0, 'tap') + ' in the database</span></button>';
    S.unassigned.forEach(function (d) {
      list += '<button class="session" data-action="pick" data-id="day:' + esc(d.date) + '"' + (S.sel === 'day:' + d.date ? ' aria-current="true"' : '') + '><b>No lecture set</b><span>' +
        esc(d.date) + ' · ' + esc(d.first) + '–' + esc(d.last) + '<br>' + plural(d.cards, 'card') + ' tapped</span></button>';
    });
    S.lectures.forEach(function (l) { list += lectureItem(l); });
    if (!S.lectures.length && !S.unassigned.length && !((S.state && S.state.counts.taps) || 0)) {
      el.innerHTML = '<div class="card">' + empty('Nothing recorded yet', 'Start a lecture, take attendance with the device, then plug it in. Everything it recorded appears here.') + '</div>';
      return;
    }
    el.innerHTML = '<div class="split"><div class="sessions" id="session-list">' + list + '</div><div id="session-detail"></div></div>';
    renderDetail();
  }

  function table(rows, cols) {
    if (!rows.length) { return '<div class="tablewrap">' + empty('Nothing to show here') + '</div>'; }
    var h = '<div class="tablewrap"><table><thead><tr>' + cols.map(function (c) { return '<th' + (c.th ? ' class="' + c.th + '"' : '') + '>' + esc(c.h) + '</th>'; }).join('') + '</tr></thead><tbody>';
    rows.forEach(function (r) { h += '<tr>' + cols.map(function (c) { return '<td' + (c.cls ? ' class="' + c.cls + '"' : '') + '>' + c.f(r) + '</td>'; }).join('') + '</tr>'; });
    return h + '</tbody></table></div>';
  }

  function matches(qs, parts) {
    if (!qs) { return true; }
    qs = qs.toLowerCase();
    return parts.some(function (p) { return String(p == null ? '' : p).toLowerCase().indexOf(qs) >= 0; });
  }

  function renderDetail() {
    var box = $('#session-detail');
    if (!box) { return; }
    if (S.sel === null) {
      box.innerHTML = '<div class="card">' + empty('Pick a lecture', 'Choose one on the left to see who came.') + '</div>';
      return;
    }
    if (S.sel === 'taps') {
      box.innerHTML = '<div class="card stack"><div class="row between"><div><h2>Every tap</h2><div class="muted">The newest 500, from every lecture and none</div></div>' +
        '<button class="btn" data-action="csv" data-path="/api/taps.csv">Download all as CSV</button></div>' +
        '<input type="search" id="search" placeholder="Search name or card" value="' + esc(S.search) + '" aria-label="Search"><div id="table-slot"></div></div>';
      drawTable();
      return;
    }
    if (typeof S.sel === 'string' && S.sel.indexOf('day:') === 0) {
      var day = S.sel.slice(4);
      var d = S.unassigned.filter(function (x) { return x.date === day; })[0];
      if (!d) { box.innerHTML = ''; return; }
      box.innerHTML = '<div class="card stack"><h2>Taps with no lecture set</h2><div class="muted">' + esc(d.date) + ' · ' + esc(d.first) + '–' + esc(d.last) + ' · ' + plural(d.cards, 'card') + ', ' + plural(d.taps, 'tap') + '</div>' +
        '<p>These were taken while no lecture was running. Turn them into a lecture and they are counted.</p>' +
        '<div class="grid two"><label class="field"><span>Module</span><input type="text" id="u-module" list="modules2" maxlength="24" value="' + esc(S.form.module) + '"><datalist id="modules2">' + S.modules.map(function (m) { return '<option value="' + esc(m.code) + '">'; }).join('') + '</datalist></label>' +
        '<label class="field"><span>Lecture name</span><input type="text" id="u-title" maxlength="32" value=""></label></div>' +
        '<div class="row"><button class="btn primary" data-action="make-lecture" data-date="' + esc(d.date) + '">Make this a lecture</button></div></div>';
      return;
    }
    var a = S.detail;
    if (!a) { box.innerHTML = '<div class="card">' + empty('Loading…') + '</div>'; return; }
    var l = a.lecture, c = a.counts;
    var lists = filterLists(a);
    box.innerHTML = '<div class="card stack">' +
      '<div class="row between"><div><h2>' + esc(l.title) + '</h2><div class="muted">' + esc(l.module_code) + (a.module_title ? ' · ' + esc(a.module_title) : '') + ' · ' + esc(l.date) + ' · ' + esc(l.start_text.slice(11, 16)) + '–' + esc(l.end_text.slice(11, 16)) + (l.running ? ' (running)' : '') + '</div></div>' +
      '<div class="sticky-actions"><button class="btn primary" data-action="pdf" data-path="/api/lectures/' + l.id + '.pdf">Download PDF</button>' +
      '<button class="btn" data-action="csv" data-path="/api/lectures/' + l.id + '.csv">Download CSV</button>' +
      '<button class="btn" data-action="print" data-path="/api/lectures/' + l.id + '.html">Print</button></div></div>' +
      '<div class="stats">' +
        '<div class="stat ok"><b>' + c.present_enrolled + '</b><span>present</span></div>' +
        '<div class="stat bad"><b>' + c.absent + '</b><span>absent</span></div>' +
        '<div class="stat"><b>' + c.percent + '%</b><span>of ' + c.enrolled + ' enrolled</span></div>' +
        (c.present_other ? '<div class="stat"><b>' + c.present_other + '</b><span>not enrolled here</span></div>' : '') +
        (c.unregistered ? '<div class="stat"><b>' + c.unregistered + '</b><span>unregistered cards</span></div>' : '') + '</div>' +
      '<div class="bar" aria-hidden="true"><i style="width:' + Math.min(100, c.percent) + '%"></i></div>' +
      '<div class="row between"><div class="seg" role="group" aria-label="Show">' +
        [['present', 'Present', c.present_enrolled], ['absent', 'Absent', c.absent], ['other', 'Not enrolled', c.present_other], ['unregistered', 'Unregistered', c.unregistered]].map(function (v) {
          return '<button data-action="view" data-v="' + v[0] + '" aria-pressed="' + (S.view === v[0]) + '">' + v[1] + ' (' + v[2] + ')</button>';
        }).join('') + '</div>' +
      '<input type="search" id="search" style="max-width:260px" placeholder="Search name or card" value="' + esc(S.search) + '" aria-label="Search"></div>' +
      '<div id="table-slot">' + detailTable(lists) + '</div>' +
      '<div class="row"><button class="btn sm" data-action="edit-lecture" data-id="' + l.id + '">Edit lecture…</button>' +
      (l.running ? '<button class="btn sm" data-action="end-lecture" data-id="' + l.id + '">End lecture</button>' : '') +
      '<button class="btn sm danger" data-action="delete-lecture" data-id="' + l.id + '">Delete lecture</button></div></div>';
  }

  function filterLists(a) {
    var qs = S.search;
    var f = function (arr) { return arr.filter(function (p) { return matches(qs, [p.name, p.student_no, p.department, p.card_id]); }); };
    return { present: f(a.present), absent: f(a.absent), other: f(a.present_other), unregistered: a.unregistered.filter(function (u) { return matches(qs, [u.card_id]); }) };
  }

  function studentCols(extra) {
    return [{ h: 'Name', f: function (r) { return esc(r.name); } }, { h: 'Student no', f: function (r) { return esc(r.student_no); } },
            { h: 'Department', f: function (r) { return esc(r.department); } },
            { h: 'Card', f: function (r) { return '<span class="mono">' + card10(r.card_id) + '</span>'; } }].concat(extra || []);
  }

  function detailTable(lists) {
    if (S.view === 'absent') { return table(lists.absent, studentCols([{ h: 'Status', f: function () { return '<span class="tag bad">Absent</span>'; } }])); }
    if (S.view === 'other') {
      return table(lists.other, studentCols([{ h: 'Arrived', f: function (r) { return esc(r.time); }, cls: 'num' },
        { h: '', f: function (r) { return '<button class="btn sm" data-action="edit-student" data-id="' + r.card_id + '">Edit modules</button>'; } }]));
    }
    if (S.view === 'unregistered') {
      return table(lists.unregistered, [{ h: 'Card', f: function (r) { return '<span class="mono">' + card10(r.card_id) + '</span>'; } },
        { h: 'Time', f: function (r) { return esc(r.time); }, cls: 'num' },
        { h: '', f: function (r) { return '<button class="btn sm primary" data-action="register" data-id="' + r.card_id + '">Register this card</button>'; } }]);
    }
    return table(lists.present, studentCols([{ h: 'Arrived', f: function (r) { return esc(r.time); }, cls: 'num' }, { h: 'Status', f: function () { return '<span class="tag ok">Present</span>'; } }]));
  }

  function drawTable() {
    var slot = $('#table-slot');
    if (!slot) { return; }
    if (S.sel === 'taps') {
      var rows = S.taps.filter(function (t) { return matches(S.search, [t.name, t.student_no, t.department, t.card_id, t.time]); });
      slot.innerHTML = table(rows, [{ h: 'Time', f: function (t) { return esc(t.time); }, cls: 'nowrap num' },
        { h: 'Card', f: function (t) { return '<span class="mono">' + card10(t.card_id) + '</span>'; } },
        { h: 'Student', f: function (t) { return t.name ? esc(t.name) : '<span class="tag warn">Unregistered</span>'; } },
        { h: 'Department', f: function (t) { return esc(t.department || ''); } }]);
      return;
    }
    if (S.detail) { slot.innerHTML = detailTable(filterLists(S.detail)); }
  }

  function selectLecture(id) {
    S.sel = id;
    S.search = '';
    S.detail = null;
    renderAttendance();
    refreshSelected().then(function () { renderAttendance(); });
  }

  function lectureDialog(id) {
    var l = S.lectures.filter(function (x) { return x.id === id; })[0] || (S.detail && S.detail.lecture);
    if (!l) { return; }
    openDialog('<h3>Edit lecture</h3><div class="form-grid">' +
      '<label class="field"><span>Module</span><input type="text" id="ld-module" list="modules3" value="' + esc(l.module_code) + '" maxlength="24"><datalist id="modules3">' + S.modules.map(function (m) { return '<option value="' + esc(m.code) + '">'; }).join('') + '</datalist></label>' +
      '<label class="field"><span>Name</span><input type="text" id="ld-title" value="' + esc(l.title) + '" maxlength="80"></label>' +
      '<label class="field"><span>Starts</span><input type="text" id="ld-start" value="' + esc(l.start_text) + '" placeholder="2026-10-06 09:00"></label>' +
      '<label class="field"><span>Ends</span><input type="text" id="ld-end" value="' + esc(l.running ? '' : l.end_text) + '" placeholder="leave empty if still running"></label>' +
      '<div class="wide hint">Taps between the start and the end count as this lecture. Dates look like 2026-10-06 09:00.</div></div>' +
      '<div class="row end"><button class="btn" data-action="close-dialog">Cancel</button><button class="btn primary" data-action="save-lecture" data-id="' + l.id + '">Save</button></div>');
  }

  /* ---------------------------------------------------- tab: students */
  function deviceCardsLine() {
    var cs = S.state && S.state.connected && S.state.cards;
    if (!cs || cs.device === null) { return ''; }
    return '<span class="small ' + (cs.in_sync ? 'muted' : '') + '">' +
      (cs.in_sync ? 'The device knows all ' + cs.device + ' cards.' : 'The device knows ' + cs.device + ' of ' + cs.database + ' cards. Send them so new cards show green.') + '</span>';
  }

  function renderStudents() {
    var el = $('#tab-students');
    var fl = S.filter;
    var deptOpts = '<option value="">All departments</option>' + S.departments.map(function (d) { return '<option' + (d === fl.department ? ' selected' : '') + '>' + esc(d) + '</option>'; }).join('');
    var modOpts = '<option value="">All modules</option>' + S.modules.map(function (m) { return '<option value="' + esc(m.code) + '"' + (m.code === fl.module ? ' selected' : '') + '>' + esc(m.code) + '</option>'; }).join('');

    var cards = '';
    if (S.cards.length) {
      cards = '<div class="card newcards"><h3>New cards seen by the device (' + S.cards.length + ')</h3>' +
        '<p class="muted">These were tapped but belong to nobody yet. Register each one to give it a name, department and modules.</p>' +
        S.cards.slice(0, 8).map(function (c) {
          return '<div class="item"><span class="mono">' + card10(c.card_id) + '</span><span class="muted small">tapped ' + plural(c.taps, 'time') + ', last ' + esc(c.last) + '</span><span class="spacer"></span>' +
            '<button class="btn sm primary" data-action="register" data-id="' + c.card_id + '">Register</button></div>';
        }).join('') + (S.cards.length > 8 ? '<div class="muted small">…and ' + (S.cards.length - 8) + ' more.</div>' : '') + '</div>';
    }

    el.innerHTML = cards +
      '<div class="card stack"><div class="row between"><div><h2>Students</h2><div class="muted" id="st-count"></div><div id="st-devline">' + deviceCardsLine() + '</div></div>' +
      '<div class="row"><button class="btn primary" data-action="add-student">+ Add student</button>' +
      '<button class="btn" data-action="import-students">Import…</button>' +
      '<button class="btn" id="st-send" data-action="send-cards"' + (S.online && S.state && S.state.connected ? '' : ' disabled') + ' title="Give the device the card numbers, so it can show green or red">Send cards to device</button>' +
      '<button class="btn" data-action="csv" data-path="/api/students.csv">Export CSV</button></div></div>' +
      '<div class="row"><input type="search" id="st-q" style="flex:1;min-width:12rem" placeholder="Search name, number, card or department" value="' + esc(fl.q) + '" aria-label="Search students">' +
      '<select id="st-module" style="max-width:14rem" aria-label="Module">' + modOpts + '</select><select id="st-dept" style="max-width:16rem" aria-label="Department">' + deptOpts + '</select></div>' +
      '<div id="st-table"></div>' +
      (!S.students.length ? '' : '') + '</div>' +
      (!S.cards.length ? '<div class="card"><h3>Adding a new student</h3><ol class="steps"><li>Tap the new student’s card on the device.</li><li>Plug the device into this computer.</li>' +
        '<li>The card appears here under “New cards”. Press <b>Register</b>, fill in the details and save.</li>' +
        '<li>Press <b>Send cards to device</b>, eject and unplug, so the device shows green for the new card.</li></ol></div>' : '');
    drawStudents();
  }

  function drawStudents() {
    var slot = $('#st-table');
    if (!slot) { return; }
    var fl = S.filter;
    var rows = S.students.filter(function (s) {
      return (!fl.module || s.modules.indexOf(fl.module) >= 0) && (!fl.department || s.department === fl.department) &&
             matches(fl.q, [s.name, s.student_no, s.department, s.card_id, card10(s.card_id)]);
    });
    var cnt = $('#st-count');
    if (cnt) { cnt.textContent = rows.length === S.students.length ? plural(S.students.length, 'student') : rows.length + ' of ' + plural(S.students.length, 'student'); }
    if (!S.students.length) {
      slot.innerHTML = empty('No students yet', 'Add one, import a class list from Excel, or tap a card on the device and register it when it appears.');
      return;
    }
    slot.innerHTML = table(rows, [
      { h: 'Name', f: function (s) { return esc(s.name); } },
      { h: 'Student no', f: function (s) { return esc(s.student_no); } },
      { h: 'Department', f: function (s) { return esc(s.department); } },
      { h: 'Modules', f: function (s) { return s.modules.length ? '<div class="chips">' + s.modules.map(function (m) { return '<span class="chip">' + esc(m) + '</span>'; }).join('') + '</div>' : '<span class="muted">none</span>'; } },
      { h: 'Card', f: function (s) { return '<span class="mono">' + card10(s.card_id) + '</span>'; } },
      { h: 'Last tap', f: function (s) { return esc(s.last_tap_text || '—'); }, cls: 'nowrap num' },
      { h: '', f: function (s) { return '<button class="btn sm" data-action="edit-student" data-id="' + s.card_id + '">Edit</button>'; } }]);
  }

  function studentDialog(card, prefill) {
    var s = card ? S.students.filter(function (x) { return x.card_id === card; })[0] : null;
    var v = s || prefill || {};
    var mods = S.modules.map(function (m) { return m.code; });
    (v.modules || []).forEach(function (m) { if (mods.indexOf(m) < 0) { mods.push(m); } });
    var checks = mods.map(function (m) {
      return '<label class="check"><input type="checkbox" data-mod="' + esc(m) + '"' + ((v.modules || []).indexOf(m) >= 0 ? ' checked' : '') + '><span>' + esc(m) + '</span></label>';
    }).join('');
    var pick = '';
    if (!s && S.cards.length) {
      pick = '<select id="sd-pick" aria-label="Cards seen by the device"><option value="">Pick a card the device saw…</option>' +
        S.cards.map(function (c) { return '<option value="' + c.card_id + '">' + card10(c.card_id) + ' (last ' + esc(c.last.slice(5, 16)) + ')</option>'; }).join('') + '</select>';
    }
    openDialog('<h3>' + (s ? 'Edit student' : 'Register a student') + '</h3><div class="form-grid">' +
      '<div class="wide"><label class="field"><span>Card number</span><div class="cardbox"><input type="text" id="sd-card" inputmode="numeric" autocomplete="off" value="' + esc(v.card_id ? card10(v.card_id) : '') + '"' + (s ? ' readonly' : '') + ' placeholder="e.g. 0000123456">' +
        (!s ? '<button class="btn" type="button" data-action="wait-card">Read from device</button>' : '') + '</div></label>' +
        (pick ? '<div style="margin-top:.4rem">' + pick + '</div>' : '') +
        '<div class="waiting" id="sd-waiting" hidden style="margin-top:.5rem"><b>Waiting for a new card…</b> Tap it on the device, then plug the device into this computer. The number will appear here.</div>' +
        '<div class="err" id="sd-card-err"></div></div>' +
      '<label class="field wide"><span>Full name</span><input type="text" id="sd-name" maxlength="80" autocomplete="off" value="' + esc(v.name || '') + '"></label>' +
      '<label class="field"><span>Student number</span><input type="text" id="sd-no" maxlength="40" autocomplete="off" value="' + esc(v.student_no || '') + '" placeholder="e.g. EN/21/045"></label>' +
      '<label class="field"><span>Department</span><input type="text" id="sd-dept" list="depts" maxlength="80" autocomplete="off" value="' + esc(v.department || '') + '"><datalist id="depts">' + S.departments.map(function (d) { return '<option value="' + esc(d) + '">'; }).join('') + '</datalist></label>' +
      '<div class="wide"><span class="small" style="font-weight:600">Enrolled modules</span><div class="checks" id="sd-mods" style="margin-top:.3rem">' + (checks || '<span class="muted">No modules yet. Add one below.</span>') + '</div>' +
        '<div class="row" style="margin-top:.4rem"><input type="text" id="sd-newmod" maxlength="24" placeholder="Add another module code" style="max-width:15rem"><button class="btn sm" type="button" data-action="add-mod-check">Add</button></div></div>' +
      '</div><div class="err" id="sd-err" style="margin-top:.5rem"></div>' +
      '<div class="row end">' + (s ? '<button class="btn danger" data-action="delete-student" data-id="' + s.card_id + '" style="margin-right:auto">Delete student</button>' : '') +
      '<button class="btn" data-action="close-dialog">Cancel</button><button class="btn primary" data-action="save-student">Save</button></div>');
    if (!s) { var n = $('#sd-card'); if (v.card_id) { $('#sd-name').focus(); } else { n.focus(); } }
  }

  function saveStudent() {
    var mods = $$('#sd-mods input[data-mod]').filter(function (c) { return c.checked; }).map(function (c) { return c.dataset.mod; });
    $('#sd-err').textContent = '';
    api('POST', '/api/students', { card_id: $('#sd-card').value, name: $('#sd-name').value, student_no: $('#sd-no').value,
                                   department: $('#sd-dept').value, modules: mods })
      .then(function (s) { closeDialog(); toast(s.name + ' saved. Send the cards to the device so it shows green for this card.'); return loadAll(); })
      .then(function () { render(); poll(); })
      .catch(function (e) { $('#sd-err').textContent = e.message; });
  }

  function importDialog() {
    openDialog('<h3>Import students</h3><p>Paste rows from Excel, or choose a CSV file. The first line can name the columns:</p>' +
      '<p class="mono small">card_id, name, student_no, department, modules</p>' +
      '<p class="small muted">Modules are separated by semicolons, like <span class="mono">EN2090;MA1010</span>. Students already in the list are updated.</p>' +
      '<textarea id="im-text" rows="9" spellcheck="false" placeholder="0000123456&#9;Alice Perera&#9;EN/21/001&#9;Electrical Engineering&#9;EN2090;MA1010"></textarea>' +
      '<div class="row" style="margin-top:.5rem"><button class="btn sm" type="button" data-action="pick-import-file">Choose a file…</button><span class="small muted" id="im-file"></span></div>' +
      '<div id="im-result" class="small" style="margin-top:.5rem"></div>' +
      '<div class="row end"><button class="btn" data-action="close-dialog">Close</button><button class="btn primary" data-action="run-import">Import</button></div>');
  }

  /* ----------------------------------------------------- tab: modules */
  function renderModules() {
    var el = $('#tab-modules');
    el.innerHTML = '<div class="card stack"><div class="row between"><div><h2>Modules</h2><div class="muted">The courses you teach, and who is enrolled in each</div></div>' +
      '<button class="btn primary" data-action="add-module">+ Add module</button></div>' +
      (S.modules.length ? table(S.modules, [
        { h: 'Code', f: function (m) { return '<b>' + esc(m.code) + '</b>'; } },
        { h: 'Title', f: function (m) { return esc(m.title); } },
        { h: 'Department', f: function (m) { return esc(m.department); } },
        { h: 'Students', f: function (m) { return m.students; }, cls: 'num', th: 'c' },
        { h: 'Lectures', f: function (m) { return m.lectures; }, cls: 'num', th: 'c' },
        { h: '', f: function (m) { return '<button class="btn sm" data-action="enrol-module" data-code="' + esc(m.code) + '">Enrolled students</button> <button class="btn sm" data-action="edit-module" data-code="' + esc(m.code) + '">Edit</button>'; } }])
        : empty('No modules yet', 'Add a module, then enrol students in it. Modules are also created when you start a lecture or register a student.')) + '</div>';
  }

  function moduleDialog(code) {
    var m = S.modules.filter(function (x) { return x.code === code; })[0] || { code: '', title: '', department: '' };
    openDialog('<h3>' + (code ? 'Edit module' : 'Add a module') + '</h3><div class="form-grid">' +
      '<label class="field"><span>Code</span><input type="text" id="md-code" maxlength="24" value="' + esc(m.code) + '" placeholder="e.g. EN2090"></label>' +
      '<label class="field"><span>Department</span><input type="text" id="md-dept" list="depts2" maxlength="80" value="' + esc(m.department) + '"><datalist id="depts2">' + S.departments.map(function (d) { return '<option value="' + esc(d) + '">'; }).join('') + '</datalist></label>' +
      '<label class="field wide"><span>Title</span><input type="text" id="md-title" maxlength="80" value="' + esc(m.title) + '" placeholder="e.g. Circuits and Systems"></label></div>' +
      '<div class="err" id="md-err" style="margin-top:.5rem"></div>' +
      '<div class="row end">' + (code ? '<button class="btn danger" style="margin-right:auto" data-action="delete-module" data-code="' + esc(code) + '">Delete…</button>' : '') +
      '<button class="btn" data-action="close-dialog">Cancel</button><button class="btn primary" data-action="save-module" data-old="' + esc(code || '') + '">Save</button></div>');
  }

  function enrolDialog(code) {
    var m = S.modules.filter(function (x) { return x.code === code; })[0];
    if (!m) { return; }
    var rows = S.students.map(function (s) {
      return '<label class="check" data-name="' + esc((s.name + ' ' + s.student_no + ' ' + s.department).toLowerCase()) + '"><input type="checkbox" data-card="' + s.card_id + '"' + (s.modules.indexOf(code) >= 0 ? ' checked' : '') + '><span>' + esc(s.name) + (s.student_no ? ' <span class="muted small">' + esc(s.student_no) + '</span>' : '') + '</span></label>';
    }).join('');
    openDialog('<h3>Students enrolled in ' + esc(code) + '</h3>' +
      '<div class="row"><input type="search" id="en-q" placeholder="Search" style="flex:1" aria-label="Search students">' +
      '<button class="btn sm" data-action="en-all">Tick shown</button><button class="btn sm" data-action="en-none">Clear shown</button></div>' +
      '<div class="checks" id="en-list" style="max-height:22rem;margin-top:.5rem;grid-template-columns:repeat(auto-fill,minmax(230px,1fr))">' + (rows || '<span class="muted">No students yet.</span>') + '</div>' +
      '<div class="small muted" id="en-count" style="margin-top:.4rem"></div><div class="err" id="en-err"></div>' +
      '<div class="row end"><button class="btn" data-action="close-dialog">Cancel</button><button class="btn primary" data-action="save-enrol" data-code="' + esc(code) + '">Save</button></div>');
    updateEnrolCount();
  }

  function updateEnrolCount() {
    var all = $$('#en-list input'); var n = all.filter(function (c) { return c.checked; }).length;
    var el = $('#en-count');
    if (el) { el.textContent = n + ' of ' + all.length + ' ticked'; }
  }

  /* ---------------------------------------------------- tab: reports */
  function renderReports() {
    var el = $('#tab-reports');
    var modeBtn = function (m, label) { return '<button data-action="rep-mode" data-m="' + m + '" aria-pressed="' + (S.repMode === m) + '">' + label + '</button>'; };
    var head = '<div class="card stack"><div class="row between"><div><h2>Reports</h2><div class="muted">Taps matched to your students, as a page you can print or a file you can keep</div></div>' +
      '<div class="seg" role="group">' + modeBtn('module', 'By module') + modeBtn('student', 'By student') + '</div></div>';
    if (S.repMode === 'student') {
      var opts = '<option value="">Choose a student…</option>' + S.students.map(function (s) { return '<option value="' + s.card_id + '"' + (String(s.card_id) === String(S.rep.card) ? ' selected' : '') + '>' + esc(s.name) + (s.student_no ? ' (' + esc(s.student_no) + ')' : '') + '</option>'; }).join('');
      el.innerHTML = head + '<label class="field" style="max-width:26rem"><span>Student</span><select id="r-card">' + opts + '</select></label><div id="rep-body"></div></div>';
      loadStudentReport();
      return;
    }
    if (!S.rep.module && S.modules.length) { S.rep.module = S.modules[0].code; }
    var mopts = S.modules.map(function (m) { return '<option value="' + esc(m.code) + '"' + (m.code === S.rep.module ? ' selected' : '') + '>' + esc(m.code + (m.title ? ' — ' + m.title : '')) + '</option>'; }).join('');
    el.innerHTML = head + (S.modules.length
      ? '<div class="row"><label class="field"><span>Module</span><select id="r-module">' + mopts + '</select></label>' +
        '<label class="field"><span>From</span><input type="date" id="r-from" value="' + esc(S.rep.from) + '"></label>' +
        '<label class="field"><span>To</span><input type="date" id="r-to" value="' + esc(S.rep.to) + '"></label></div><div id="rep-body"></div>'
      : empty('No modules yet', 'Add a module and some lectures first.')) + '</div>';
    if (S.modules.length) { loadModuleReport(); }
  }

  function loadModuleReport() {
    var body = $('#rep-body');
    api('GET', '/api/reports/module/' + encodeURIComponent(S.rep.module) + q({ from: S.rep.from, to: S.rep.to })).then(function (r) {
      S.repData = r;
      var s = r.summary;
      var base = '/api/reports/module/' + encodeURIComponent(S.rep.module);
      var qs = q({ from: S.rep.from, to: S.rep.to });
      var th = '<th>Name</th><th>Student no</th>' + r.lectures.map(function (l) { return '<th class="c" title="' + esc(l.title) + '">' + esc(l.date.slice(5)) + '<br><span style="text-transform:none;letter-spacing:0">' + esc(l.title) + '</span></th>'; }).join('') + '<th class="c">Present</th><th class="c">%</th>';
      var trs = r.students.map(function (st) {
        return '<tr><td>' + esc(st.name) + '</td><td>' + esc(st.student_no) + '</td>' + st.flags.map(function (f) { return '<td class="' + (f ? 'cellP' : 'cellA') + '">' + (f ? 'P' : '–') + '</td>'; }).join('') +
          '<td class="c num">' + st.count + '/' + s.lectures + '</td><td class="c num' + (st.percent < 75 ? ' low' : '') + '">' + st.percent + '%</td></tr>';
      }).join('');
      body.innerHTML = '<div class="stats"><div class="stat"><b>' + s.lectures + '</b><span>lectures</span></div><div class="stat"><b>' + s.students + '</b><span>enrolled students</span></div>' +
        '<div class="stat ok"><b>' + s.average + '%</b><span>average attendance</span></div><div class="stat bad"><b>' + s.below_75 + '</b><span>below 75%</span></div></div>' +
        '<div class="sticky-actions"><button class="btn primary" data-action="pdf" data-path="' + base + '.pdf' + qs + '">Download PDF</button>' +
        '<button class="btn" data-action="csv" data-path="' + base + '.csv' + qs + '">Download CSV</button>' +
        '<button class="btn" data-action="print" data-path="' + base + '.html' + qs + '">Print</button></div>' +
        (r.lectures.length && r.students.length ? '<div class="tablewrap" style="margin-top:.75rem"><table><thead><tr>' + th + '</tr></thead><tbody>' + trs + '</tbody></table></div>'
          : '<div class="tablewrap" style="margin-top:.75rem">' + empty(r.students.length ? 'No lectures in this range' : 'Nobody is enrolled in this module', r.students.length ? 'Change the dates, or start a lecture for this module.' : 'Use Modules → Enrolled students.') + '</div>');
    }).catch(function (e) { body.innerHTML = '<div class="err">' + esc(e.message) + '</div>'; });
  }

  function loadStudentReport() {
    var body = $('#rep-body');
    if (!S.rep.card) { body.innerHTML = ''; return; }
    api('GET', '/api/reports/student/' + S.rep.card).then(function (r) {
      var base = '/api/reports/student/' + S.rep.card;
      var st = r.student;
      body.innerHTML = '<div class="row between" style="margin-top:.75rem"><div><h3>' + esc(st.name) + '</h3><div class="muted">Card ' + card10(st.card_id) + ' · ' + esc(st.student_no || 'no student number') + ' · ' + esc(st.department || 'no department') + '<br>' + plural(r.taps, 'tap') + ' recorded' + (r.last_tap ? ', last ' + esc(r.last_tap) : '') + '</div></div>' +
        '<div class="sticky-actions"><button class="btn primary" data-action="pdf" data-path="' + base + '.pdf">Download PDF</button><button class="btn" data-action="csv" data-path="' + base + '.csv">Download CSV</button><button class="btn" data-action="print" data-path="' + base + '.html">Print</button></div></div>' +
        (r.modules.length ? r.modules.map(function (m) {
          return '<h3 style="margin-top:1rem">' + esc(m.code) + ' ' + esc(m.title) + ' — ' + m.attended + ' of ' + m.total + ' (' + m.percent + '%)</h3>' +
            table(m.lectures, [{ h: 'Date', f: function (l) { return esc(l.date); }, cls: 'num' }, { h: 'Lecture', f: function (l) { return esc(l.title); } },
              { h: 'Attended', f: function (l) { return l.attended ? '<span class="tag ok">Yes</span>' : '<span class="tag bad">No</span>'; } }, { h: 'Arrived', f: function (l) { return esc(l.time); }, cls: 'num' }]);
        }).join('') : '<div class="tablewrap" style="margin-top:.75rem">' + empty('Not enrolled in any module', 'Enrol this student in a module to see their attendance.') + '</div>');
    }).catch(function (e) { body.innerHTML = '<div class="err">' + esc(e.message) + '</div>'; });
  }

  /* ----------------------------------------------------------- events */
  document.addEventListener('click', function (e) {
    var tab = e.target.closest('#tabs button');
    if (tab) { S.tab = tab.dataset.tab; history.replaceState(null, '', '#' + S.tab); render.keep = false; loadAll().then(function () { return refreshSelected(); }).then(render); render(); return; }
    var b = e.target.closest('[data-action]');
    if (!b) { return; }
    var a = b.dataset.action;
    var id = b.dataset.id !== undefined ? b.dataset.id : null;
    var num = id !== null && /^-?\d+$/.test(id) ? parseInt(id, 10) : id;
    switch (a) {
      case 'dismiss': S.dismissed[b.dataset.key] = true; renderBanners(); break;
      case 'close-dialog': closeDialog(); break;
      case 'quit':
        api('POST', '/api/quit', {}).then(function () {
          document.body.innerHTML = '<main style="max-width:30rem;margin:4rem auto;text-align:center"><h2>The app has stopped</h2><p class="muted">You can close this tab. Start it again with attendance_app.py.</p></main>';
        });
        break;
      case 'suggest': S.form.title = suggestTitle(S.form.module); S.form.touched = true; render.keep = false; renderLecture(); break;
      case 'start': startLecture(); break;
      case 'record-only':
        api('POST', '/api/lectures', { module: S.form.module, title: S.form.title, start: S.state.now }).then(function (l) {
          toast('Recorded “' + l.title + '” here only'); S.form.touched = false; return loadAll();
        }).then(function () { render(); poll(); }).catch(fail);
        break;
      case 'end-lecture':
        api('POST', '/api/lectures/' + num + '/end', {}).then(function () { toast('Lecture ended'); return loadAll(); }).then(function () { return refreshSelected(); }).then(function () { poll(); render(); }).catch(fail);
        break;
      case 'open-lecture': S.tab = 'attendance'; selectLecture(num); history.replaceState(null, '', '#attendance'); render(); break;
      case 'pick': selectLecture(/^\d+$/.test(id) ? parseInt(id, 10) : id); break;
      case 'view': S.view = b.dataset.v; renderDetail(); break;
      case 'csv': case 'pdf': download(b.dataset.path); break;
      case 'print': printPage(b.dataset.path); break;
      case 'edit-lecture': lectureDialog(num); break;
      case 'save-lecture':
        api('PATCH', '/api/lectures/' + num, { module: $('#ld-module').value, title: $('#ld-title').value, start: $('#ld-start').value, end: $('#ld-end').value })
          .then(function () { closeDialog(); toast('Lecture saved'); return loadAll(); }).then(function () { return refreshSelected(); }).then(render).catch(fail);
        break;
      case 'delete-lecture':
        if (window.confirm('Delete this lecture? The taps stay in the database; they just stop counting as this lecture.')) {
          api('DELETE', '/api/lectures/' + num).then(function () { S.sel = null; S.detail = null; toast('Lecture deleted'); return loadAll(); }).then(render).catch(fail);
        }
        break;
      case 'make-lecture':
        api('POST', '/api/unassigned/lecture', { date: b.dataset.date, module: $('#u-module').value, title: $('#u-title').value }).then(function (l) {
          toast('Lecture “' + l.title + '” made'); S.sel = l.id; return loadAll();
        }).then(function () { return refreshSelected(); }).then(render).catch(fail);
        break;
      case 'add-student': studentDialog(null, {}); break;
      case 'edit-student': studentDialog(num); break;
      case 'register': {
        var cardNo = num;
        studentDialog(null, { card_id: cardNo, modules: S.state && S.state.current_lecture ? [S.state.current_lecture.module_code] : [] });
        break;
      }
      case 'wait-card':
        S.waiting = { known: S.cards.map(function (c) { return c.card_id; }) };
        $('#sd-waiting').hidden = false;
        break;
      case 'add-mod-check': {
        var inp = $('#sd-newmod');
        var code = inp.value.trim();
        if (code) {
          var box = $('#sd-mods');
          if (!$('#sd-mods input[data-mod="' + code.replace(/"/g, '') + '"]')) {
            var lab = document.createElement('label');
            lab.className = 'check';
            lab.innerHTML = '<input type="checkbox" data-mod="' + esc(code) + '" checked><span>' + esc(code) + '</span>';
            box.appendChild(lab);
          }
          inp.value = '';
        }
        break;
      }
      case 'save-student': saveStudent(); break;
      case 'delete-student':
        if (window.confirm('Delete this student? Their taps stay in the database but will show as an unregistered card.')) {
          api('DELETE', '/api/students/' + num).then(function () { closeDialog(); toast('Student deleted'); return loadAll(); }).then(render).catch(fail);
        }
        break;
      case 'import-students': importDialog(); break;
      case 'pick-import-file': {
        var fi = document.createElement('input');
        fi.type = 'file'; fi.accept = '.csv,.txt,text/csv,text/plain';
        fi.onchange = function () { if (fi.files.length) { fi.files[0].text().then(function (t) { $('#im-text').value = t; $('#im-file').textContent = fi.files[0].name; }); } };
        fi.click();
        break;
      }
      case 'run-import':
        api('POST', '/api/students/import', { text: $('#im-text').value }).then(function (r) {
          $('#im-result').innerHTML = '<b>' + plural(r.added, 'student') + ' added, ' + r.updated + ' updated.</b>' +
            (r.errors.length ? '<div class="err">' + r.errors.slice(0, 8).map(function (x) { return 'Line ' + x.line + ': ' + esc(x.message); }).join('<br>') + (r.errors.length > 8 ? '<br>…and ' + (r.errors.length - 8) + ' more' : '') + '</div>' : '');
          return loadAll();
        }).then(render).catch(function (e) { $('#im-result').innerHTML = '<span class="err">' + esc(e.message) + '</span>'; });
        break;
      case 'add-module': moduleDialog(''); break;
      case 'edit-module': moduleDialog(b.dataset.code); break;
      case 'save-module': {
        var old = b.dataset.old;
        var code2 = $('#md-code').value;
        var p = old && old !== code2.trim() ? api('POST', '/api/modules/rename', { old: old, new: code2 }) : Promise.resolve();
        p.then(function () { return api('POST', '/api/modules', { code: code2, title: $('#md-title').value, department: $('#md-dept').value }); })
          .then(function () { closeDialog(); toast('Module saved'); return loadAll(); }).then(render).catch(function (e) { $('#md-err').textContent = e.message; });
        break;
      }
      case 'delete-module':
        if (window.confirm('Delete module ' + b.dataset.code + '? Its enrolments go; students and taps stay. If it has lectures, they are removed too.')) {
          api('DELETE', '/api/modules/' + encodeURIComponent(b.dataset.code) + '?force=1').then(function () { closeDialog(); toast('Module deleted'); return loadAll(); }).then(render).catch(fail);
        }
        break;
      case 'enrol-module': enrolDialog(b.dataset.code); break;
      case 'en-all': case 'en-none':
        $$('#en-list label').forEach(function (l) { if (l.style.display !== 'none') { l.querySelector('input').checked = (a === 'en-all'); } });
        updateEnrolCount();
        break;
      case 'save-enrol': {
        var ids = $$('#en-list input').filter(function (c) { return c.checked; }).map(function (c) { return c.dataset.card; });
        api('POST', '/api/modules/' + encodeURIComponent(b.dataset.code) + '/enroll', { card_ids: ids, mode: 'set' })
          .then(function () { closeDialog(); toast('Enrolment saved'); return loadAll(); }).then(render).catch(function (e) { $('#en-err').textContent = e.message; });
        break;
      }
      case 'rep-mode': S.repMode = b.dataset.m; renderReports(); break;
      case 'import-taps': $('#file-input').click(); break;
      case 'backup': download('/api/backup.db'); break;
      case 'send-cards':
        api('POST', '/api/device/cards', {}).then(function (r) {
          toast(plural(r.count, 'card') + ' sent. Eject and unplug to apply.'); S.sent = null; poll();
        }).catch(fail);
        break;
      case 'device-clock':
        api('POST', '/api/device/clock', {}).then(function () { toast('Clock sent. Eject and unplug to apply it.'); S.sent = null; }).catch(fail);
        break;
    }
  });

  document.addEventListener('input', function (e) {
    var t = e.target;
    render.keep = true;
    if (t.id === 'f-module') { S.form.module = t.value; S.form.touched = true; }
    else if (t.id === 'f-title') { S.form.title = t.value; S.form.touched = true; }
    else if (t.id === 'search') { S.search = t.value; drawTable(); }
    else if (t.id === 'st-q') { S.filter.q = t.value; drawStudents(); }
    else if (t.id === 'en-q') {
      var qs = t.value.trim().toLowerCase();
      $$('#en-list label').forEach(function (l) { l.style.display = (!qs || l.dataset.name.indexOf(qs) >= 0) ? '' : 'none'; });
    } else if (t.id === 'sd-card') {
      var el = $('#sd-card-err');
      var v = t.value.trim();
      if (el) { el.textContent = v && !/^(0[xX][0-9a-fA-F]+|\d+)$/.test(v) ? 'A card number is digits only, like 0000123456.' : ''; }
    } else if (t.closest && t.closest('#en-list')) { updateEnrolCount(); }
  });

  document.addEventListener('change', function (e) {
    var t = e.target;
    if (t.id === 'f-sync') { S.form.sync = t.checked; }
    else if (t.id === 'st-module') { S.filter.module = t.value; drawStudents(); }
    else if (t.id === 'st-dept') { S.filter.department = t.value; drawStudents(); }
    else if (t.id === 'sd-pick') { if (t.value) { $('#sd-card').value = card10(t.value); $('#sd-card').dispatchEvent(new Event('input', { bubbles: true })); $('#sd-name').focus(); } }
    else if (t.id === 'r-module') { S.rep.module = t.value; loadModuleReport(); }
    else if (t.id === 'r-from') { S.rep.from = t.value; loadModuleReport(); }
    else if (t.id === 'r-to') { S.rep.to = t.value; loadModuleReport(); }
    else if (t.id === 'r-card') { S.rep.card = t.value; loadStudentReport(); }
    else if (t.closest && t.closest('#en-list')) { updateEnrolCount(); }
  });

  document.addEventListener('focusout', function () { render.keep = false; });

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && e.target.closest && e.target.closest('#dlg-body') && e.target.tagName === 'INPUT' && e.target.type !== 'search') {
      var primary = $('#dlg-body .btn.primary');
      if (primary && !e.target.closest('.cardbox')) { e.preventDefault(); primary.click(); }
    }
  });

  $('#file-input').addEventListener('change', function (e) {
    var f = e.target.files[0];
    e.target.value = '';
    if (!f) { return; }
    f.text().then(function (t) { return api('POST', '/api/taps/import', { text: t }); }).then(function (r) {
      toast(plural(r.new, 'new tap') + ' imported (' + r.read + ' in the file)');
      return loadAll();
    }).then(render).catch(fail);
  });

  /* ------------------------------------------------------------ start */
  var hash = (location.hash || '').replace('#', '');
  if (['lecture', 'attendance', 'students', 'modules', 'reports'].indexOf(hash) >= 0) { S.tab = hash; }
  render();
  poll();
  setInterval(poll, POLL_MS);

  window.__att = { S: S, render: render, loadAll: loadAll };      // for the browser console and the tests
})();
