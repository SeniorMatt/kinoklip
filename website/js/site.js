/* Kinoklip website: small, no dependencies. */
(function () {
  'use strict';
  var reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };

  /* Mobile menu */
  var toggle = $('.nav-toggle'), nav = $('#site-nav');
  if (toggle && nav) {
    toggle.addEventListener('click', function () {
      var open = nav.classList.toggle('open');
      toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && nav.classList.contains('open')) { nav.classList.remove('open'); toggle.setAttribute('aria-expanded', 'false'); toggle.focus(); }
    });
  }

  /* Scroll reveal: sections with data-anim start their animations when they come into view. */
  var animated = $$('[data-anim], .reveal');
  if (!('IntersectionObserver' in window) || reduce) {
    animated.forEach(function (el) { el.classList.add('is-in'); });
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (en.isIntersecting) en.target.classList.add('is-in');
        else if (en.target.hasAttribute('data-anim')) en.target.classList.remove('is-in');
      });
    }, { threshold: 0.18 });
    animated.forEach(function (el) { io.observe(el); });
  }

  /* Hero: timecode follows the playhead, title types itself. */
  var hero = $('[data-hero]');
  if (hero) {
    var tcEl = $('[data-tc]', hero), typedEl = $('[data-typed]', hero);
    var text = typedEl ? typedEl.getAttribute('data-text') : '';
    if (!reduce) {
      var t0 = Date.now(), seen = true;
      if ('IntersectionObserver' in window) new IntersectionObserver(function (e) { seen = e[0].isIntersecting; }).observe(hero);
      setInterval(function () {
        if (!seen) return;
        var t = ((Date.now() - t0) % 8000) / 1000, s = Math.floor(t), f = Math.floor((t - s) * 24);
        if (tcEl) tcEl.textContent = '01:00:0' + s + ':' + (f < 10 ? '0' : '') + f;
        if (typedEl) {
          var n;
          if (t < 3) n = Math.ceil(text.length * t / 2.2);
          else if (t < 7) n = text.length;
          else n = Math.max(0, Math.ceil(text.length * (1 - (t - 7) / 1)));
          typedEl.textContent = text.slice(0, Math.min(n, text.length));
        }
      }, 1000 / 24);
    }
  }

  /* Guide sidebar starts closed on narrow screens */
  var side = $('.side details');
  if (side && window.innerWidth < 960) side.removeAttribute('open');

  /* Page tabs */
  $$('[data-tabs]').forEach(function (root) {
    var tabs = $$('[role="tab"]', root), panels = $$('[role="tabpanel"]', root);
    function pick(i, focus) {
      tabs.forEach(function (t, j) {
        t.setAttribute('aria-selected', j === i ? 'true' : 'false');
        t.tabIndex = j === i ? 0 : -1;
      });
      panels.forEach(function (p, j) { p.hidden = j !== i; });
      if (focus) tabs[i].focus();
    }
    tabs.forEach(function (t, i) {
      t.addEventListener('click', function () { pick(i); });
      t.addEventListener('keydown', function (e) {
        var k = e.key, n = tabs.length;
        if (k === 'ArrowRight') { e.preventDefault(); pick((i + 1) % n, true); }
        else if (k === 'ArrowLeft') { e.preventDefault(); pick((i + n - 1) % n, true); }
        else if (k === 'Home') { e.preventDefault(); pick(0, true); }
        else if (k === 'End') { e.preventDefault(); pick(n - 1, true); }
      });
    });
    pick(0);
  });

  /* Key demo: J K L, I O, B and Shift+Delete on a small timeline. */
  var demo = $('[data-demo]');
  if (demo) {
    var tl = $('.demo-tl', demo), head = $('.demo-head', demo), range = $('.demo-range', demo);
    var tcOut = $('.tc', demo), msgOut = $('.demo-msg', demo), clipLayer = $('.demo-clips', demo);
    var st;
    var fresh = function () {
      return { clips: [{ id: 1, name: 'beach.mp4', start: 0, len: 45 }, { id: 2, name: 'pier.mov', start: 45, len: 30 }, { id: 3, name: 'sunset.mp4', start: 75, len: 25 }], head: 30, play: 0, inP: null, outP: null, next: 4 };
    };
    st = fresh();
    var say = function (m) { msgOut.textContent = m; };
    var draw = function () {
      var have = {};
      $$('.demo-clip', clipLayer).forEach(function (el) { have[el.getAttribute('data-id')] = el; });
      var seenIds = {};
      st.clips.forEach(function (c) {
        seenIds[c.id] = 1;
        var el = have[c.id];
        if (!el) {
          el = document.createElement('div'); el.className = 'demo-clip'; el.setAttribute('data-id', c.id);
          clipLayer.appendChild(el);
        }
        el.textContent = c.name;
        el.style.left = 'calc(' + c.start + '% + 1px)';
        el.style.width = 'calc(' + c.len + '% - 2px)';
      });
      Object.keys(have).forEach(function (id) { if (!seenIds[id]) have[id].remove(); });
      $$('.demo-clip', clipLayer).forEach(function (el, i) { el.style.background = i % 2 ? 'var(--clip-video2)' : 'var(--clip-video)'; });
      head.style.left = st.head + '%';
      head.classList.toggle('run', !!st.play);
      var a = st.inP === null ? 0 : st.inP, b = st.outP === null ? 100 : st.outP;
      if (b < a) { var x = a; a = b; b = x; }
      range.hidden = st.inP === null && st.outP === null;
      range.style.left = a + '%'; range.style.width = (b - a) + '%';
      var secs = st.head * 0.6, s = Math.floor(secs), f = Math.floor((secs - s) * 24);
      tcOut.textContent = '01:00:' + (s < 10 ? '0' : '') + s + ':' + (f < 10 ? '0' : '') + f;
    };
    var actions = {
      j: function () { st.play = -1; say('J: playing backwards'); },
      k: function () { st.play = 0; say('K: stopped'); },
      l: function () { st.play = 1; say('L: playing'); },
      i: function () { st.inP = st.head; say('I: in point set'); },
      o: function () { st.outP = st.head; say('O: out point set'); },
      b: function () {
        var h = st.head, out = [], cut = false;
        st.clips.forEach(function (c) {
          if (h > c.start + 1 && h < c.start + c.len - 1) {
            out.push({ id: c.id, name: c.name, start: c.start, len: h - c.start });
            out.push({ id: st.next++, name: c.name, start: h, len: c.start + c.len - h });
            cut = true;
          } else out.push(c);
        });
        st.clips = out;
        say(cut ? 'B: clip cut at the playhead' : 'B: move the playhead over a clip first');
      },
      del: function () {
        var h = st.head, hit = null;
        st.clips.forEach(function (c) { if (!hit && h >= c.start && h < c.start + c.len) hit = c; });
        if (!hit) { say('Nothing under the playhead'); return; }
        st.clips = st.clips.filter(function (c) { return c !== hit; }).map(function (c) {
          return c.start > hit.start ? { id: c.id, name: c.name, start: c.start - hit.len, len: c.len } : c;
        });
        say('Shift+Del: clip removed, the rest moved left');
      }
    };
    var press = function (k) {
      if (actions[k]) { actions[k](); draw(); }
    };
    $$('[data-key]', demo).forEach(function (btn) {
      btn.addEventListener('click', function () { press(btn.getAttribute('data-key')); });
    });
    var flash = function (k) {
      var btn = $('[data-key="' + k + '"]', demo);
      if (!btn) return;
      btn.classList.add('hit'); setTimeout(function () { btn.classList.remove('hit'); }, 140);
    };
    tl.addEventListener('keydown', function (e) {
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      var k = e.key.toLowerCase();
      if (k === 'delete' && e.shiftKey) k = 'del';
      else if (e.shiftKey) return;
      if (actions[k]) { e.preventDefault(); flash(k); press(k); }
    });
    $('[data-reset]', demo).addEventListener('click', function () {
      st = fresh(); say('Click a key below, or focus the timeline and press it'); draw();
    });
    // Playback runs only while the demo is on screen.
    var onScreen = true;
    if ('IntersectionObserver' in window) new IntersectionObserver(function (e) { onScreen = e[0].isIntersecting; }).observe(demo);
    setInterval(function () {
      if (!st.play || !onScreen) return;
      var h = st.head + st.play * 1.2;
      if (h > 100 || h < 0) { st.head = Math.max(0, Math.min(100, h)); st.play = 0; say('Stopped at the end'); }
      else st.head = h;
      draw();
    }, 120);
    draw();
  }

  /* Guide search */
  var gs = $('[data-search]');
  if (gs) {
    var data = JSON.parse($('#guide-data').textContent);
    var items = $$('[data-gid]');
    var groups = $$('.g-group');
    var count = $('[data-search-count]');
    var byId = {};
    data.forEach(function (d) { byId[d.id] = d; });
    var run = function () {
      var q = gs.value.toLowerCase().trim();
      var words = q ? q.split(/\s+/) : [];
      var shown = 0;
      items.forEach(function (li) {
        var d = byId[li.getAttribute('data-gid')];
        var hay = (d.title + ' ' + d.summary + ' ' + d.keywords.join(' ')).toLowerCase();
        var ok = words.every(function (w) { return hay.indexOf(w) !== -1; });
        li.hidden = !ok;
        if (ok) shown++;
      });
      groups.forEach(function (g) { g.hidden = !$$('[data-gid]:not([hidden])', g).length; });
      count.textContent = q ? (shown === 1 ? '1 article' : shown + ' articles') : '';
    };
    gs.addEventListener('input', run);
    var m = /[?&]q=([^&]*)/.exec(location.search);
    if (m) { gs.value = decodeURIComponent(m[1].replace(/\+/g, ' ')); run(); }
  }

  /* Bug report form: builds a prefilled GitHub issue address and opens it. */
  var form = $('[data-bugform]');
  if (form) {
    var base = form.getAttribute('data-issue-url');
    var build = function () {
      var v = function (n) { return (form.elements[n].value || '').trim(); };
      var p = [];
      var add = function (k, val) { if (val) p.push(encodeURIComponent(k) + '=' + encodeURIComponent(val)); };
      add('template', 'bug_report.yml');
      var what = v('actual');
      var firstLine = what.split('\n')[0].slice(0, 70);
      add('title', '[Bug] ' + firstLine);
      add('labels', 'bug');
      add('version', v('version'));
      add('environment', v('environment'));
      add('page', v('page'));
      add('steps', v('steps'));
      add('actual', what);
      add('expected', v('expected'));
      return base + '?' + p.join('&');
    };
    var out = $('[data-issue-preview]', form);
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      if (!form.reportValidity()) return;
      var url = build();
      if (out) { out.hidden = false; out.href = url; }
      window.open(url, '_blank', 'noopener');
    });
  }
})();
