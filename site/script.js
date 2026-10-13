/* KenshiKin site: language switch, scroll gauge, parallax, sand storm, dust, cursor trail, releases.
   No dependencies. Animations touch only transform and opacity (and a canvas). */
(function () {
  'use strict';

  var doc = document.documentElement;
  var REPO = 'Fisl-Gaming/KenshiKin';
  var reduceMq = window.matchMedia('(prefers-reduced-motion: reduce)');
  var fineMq = window.matchMedia('(pointer: fine)');
  var reduce = reduceMq.matches;

  function $(s, root) { return (root || document).querySelector(s); }
  function $$(s, root) { return Array.prototype.slice.call((root || document).querySelectorAll(s)); }
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }
  function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) {} return null; }

  /* ---------------- Language ---------------- */
  var TEXT = {
    ru: {
      title: 'KenshiKin — кооператив и PvP для Kenshi',
      desc: 'KenshiKin — форк KenshiMP: кооператив и PvP для Kenshi. У каждого игрока свой отряд, мир хоста общий. Бета.',
      published: 'Опубликована ',
      noPre: 'Сейчас тестовой сборки нет — все последние исправления уже в стабильной.',
      nav: 'Разделы'
    },
    en: {
      title: 'KenshiKin — co-op and PvP for Kenshi',
      desc: "KenshiKin is a fork of KenshiMP: co-op and PvP for Kenshi. Every player keeps their own squad, the host's world is shared. Beta.",
      published: 'Published ',
      noPre: 'No test build right now — the latest fixes are all in the stable one.',
      nav: 'Sections'
    }
  };
  var lang = doc.getAttribute('data-lang') === 'en' ? 'en' : 'ru';

  function setLang(l, save) {
    lang = l;
    doc.lang = l;
    doc.setAttribute('data-lang', l);
    document.title = TEXT[l].title;
    var md = $('meta[name="description"]');
    if (md) md.setAttribute('content', TEXT[l].desc);
    var nav = $('#nav');
    if (nav) nav.setAttribute('aria-label', TEXT[l].nav);
    $$('[data-set-lang]').forEach(function (b) {
      b.setAttribute('aria-pressed', b.getAttribute('data-set-lang') === l ? 'true' : 'false');
    });
    if (save) store('kk-lang', l);
    renderReleases();
    measure();
    requestTick();
  }
  $$('[data-set-lang]').forEach(function (b) {
    b.addEventListener('click', function () { setLang(b.getAttribute('data-set-lang'), true); });
  });

  /* ---------------- Mobile menu ---------------- */
  var menuBtn = $('.bar__menu');
  var nav = $('#nav');
  function closeMenu() {
    if (!nav.classList.contains('is-open')) return;
    nav.classList.remove('is-open');
    menuBtn.setAttribute('aria-expanded', 'false');
  }
  if (menuBtn && nav) {
    menuBtn.addEventListener('click', function () {
      var open = !nav.classList.contains('is-open');
      nav.classList.toggle('is-open', open);
      menuBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
      if (open) { var first = $('a', nav); if (first) first.focus(); }
    });
    nav.addEventListener('click', function (e) { if (e.target.closest('a')) closeMenu(); });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && nav.classList.contains('is-open')) { closeMenu(); menuBtn.focus(); }
    });
    document.addEventListener('click', function (e) {
      if (!e.target.closest('.bar')) closeMenu();
    });
  }

  /* ---------------- Reveal on scroll ---------------- */
  var reveals = $$('.reveal');
  if (reduce || !('IntersectionObserver' in window)) {
    reveals.forEach(function (el) { el.classList.add('is-in'); });
  } else {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        // also show what was jumped over (a restored scroll position, a jump to an anchor)
        if (en.isIntersecting || en.boundingClientRect.bottom < 0) { en.target.classList.add('is-in'); io.unobserve(en.target); }
      });
    }, { rootMargin: '0px 0px -8% 0px', threshold: 0.08 });
    reveals.forEach(function (el) { io.observe(el); });
  }

  /* ---------------- Scroll: gauge, current section, parallax, storm ---------------- */
  var fill = $('.gauge__fill');
  var ticksBox = $('.gauge__ticks');
  var sections = $$('main .sect');
  var navLinks = $$('.bar__nav a');
  var world = $('.world');
  var hero = $('.hero');
  var copy = $('[data-hero-copy]');
  var layers = $$('.land__layer').map(function (el) { return { el: el, k: parseFloat(el.getAttribute('data-depth')) || 0 }; });
  var storm = $('.storm');
  var sheets = $$('.storm__sheet');
  var haze = $('.storm__haze');
  var stormOn = false;
  var SHEET = [{ sp: 0.18, peak: 0.6 }, { sp: -0.3, peak: 0.72 }, { sp: 0.45, peak: 0.85 }];

  var m = { vh: 0, max: 1, tops: [], heroH: 0, worldH: 0 };
  var stormBell = 0;          // 0..1, how deep we are in the storm (also drives the dust)
  var lastY = window.scrollY || 0;
  var scrollVel = 0;

  function measure() {
    m.vh = window.innerHeight;
    m.max = Math.max(1, doc.scrollHeight - m.vh);
    m.tops = sections.map(function (s) { return s.getBoundingClientRect().top + window.scrollY; });
    m.heroH = hero.offsetHeight;
    m.worldH = world.offsetHeight;
    if (ticksBox) {
      ticksBox.textContent = '';
      m.tops.forEach(function (t) {
        var i = document.createElement('i');
        i.style.left = clamp((t - m.vh * 0.35) / m.max * 100, 0, 100) + '%';
        ticksBox.appendChild(i);
      });
    }
  }

  var ticking = false;
  function requestTick() {
    if (!ticking) { ticking = true; requestAnimationFrame(update); }
  }

  var currentIdx = -2;
  function update() {
    ticking = false;
    var y = window.scrollY || 0;
    var jump = Math.abs(y - lastY) > m.vh;
    scrollVel += (y - lastY);
    lastY = y;

    // a long jump can skip a block without it ever intersecting: show everything above the screen
    if (jump) {
      reveals.forEach(function (el) {
        if (!el.classList.contains('is-in') && el.getBoundingClientRect().bottom < 0) el.classList.add('is-in');
      });
    }

    // gauge
    var p = clamp(y / m.max, 0, 1);
    if (fill) fill.style.transform = 'translate3d(' + ((p - 1) * 100).toFixed(2) + '%,0,0)';

    // current section in the nav
    var idx = -1;
    for (var i = 0; i < m.tops.length; i++) if (y + m.vh * 0.35 >= m.tops[i]) idx = i;
    if (idx !== currentIdx) {
      currentIdx = idx;
      var id = idx >= 0 ? sections[idx].id : '';
      navLinks.forEach(function (a) {
        var on = a.getAttribute('href') === '#' + id;
        a.classList.toggle('is-current', on);
        if (on) a.setAttribute('aria-current', 'location'); else a.removeAttribute('aria-current');
      });
    }

    if (reduce) return;

    // parallax: only while the desert is on screen
    if (y < m.worldH) {
      for (var j = 0; j < layers.length; j++) {
        layers[j].el.style.transform = 'translate3d(0,' + (y * layers[j].k).toFixed(1) + 'px,0)';
      }
      copy.style.transform = 'translate3d(0,' + (y * 0.38).toFixed(1) + 'px,0)';
      copy.style.opacity = clamp(1 - y / (m.heroH * 0.62), 0, 1).toFixed(3);
    }

    // the storm: while the desert scrolls away, sand sweeps across the whole screen and clears
    var t = clamp((y - m.heroH * 0.2) / (m.heroH * 0.8), 0, 1);
    var on = t > 0 && t < 1;
    stormBell = on ? Math.pow(Math.sin(Math.PI * t), 1.4) : 0;
    if (on !== stormOn) { stormOn = on; storm.classList.toggle('is-on', on); }
    if (on) {
      var vw = window.innerWidth;
      for (var s = 0; s < sheets.length; s++) {
        var x = (t * 2 - 1) * SHEET[s].sp * vw * 1.6;
        sheets[s].style.transform = 'translate3d(' + x.toFixed(1) + 'px,0,0)';
        sheets[s].style.opacity = (stormBell * SHEET[s].peak).toFixed(3);
      }
      haze.style.opacity = (stormBell * 0.4).toFixed(3);
    }
  }

  window.addEventListener('scroll', requestTick, { passive: true });
  var resizeTimer;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () { measure(); requestTick(); dust.resize(); }, 120);
  });
  window.addEventListener('load', function () { measure(); requestTick(); });
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(function () { measure(); requestTick(); });

  /* ---------------- Dust on the desert (canvas) ---------------- */
  var mouse = { x: -9999, y: -9999, active: false };

  var dust = (function () {
    var cv = $('.dust');
    var api = { resize: function () {}, start: function () {}, stop: function () {} };
    if (!cv || reduce || !cv.getContext) return api;
    var ctx = cv.getContext('2d');
    var W = 0, H = 0, dpr = 1, parts = [], running = false, visible = true, raf = 0, last = 0, gust = 0, time = 0;

    function make(n) {
      parts = [];
      for (var i = 0; i < n; i++) {
        var d = 0.3 + Math.random() * 0.7;
        parts.push({
          x: Math.random() * W, y: Math.random() * H, d: d,
          r: 0.6 + d * d * 2.2, a: 0.18 + Math.random() * 0.5 * d,
          dark: Math.random() < 0.38, ph: Math.random() * 6.28, vx: 0, vy: 0
        });
      }
    }
    function resize() {
      var w = world.clientWidth, h = world.clientHeight;
      if (Math.abs(w - W) < 2 && Math.abs(h - H) < 140 && parts.length) return;
      W = w; H = h;
      dpr = Math.min(window.devicePixelRatio || 1, 1.5);
      cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      make(clamp(Math.round(W * H / 11000), 30, 180));
    }
    function frame(now) {
      raf = 0;
      if (!running) return;
      var dt = Math.min(48, now - (last || now)) / 16.7;
      last = now; time += dt * 0.016;

      var v = scrollVel; scrollVel = 0;
      var target = 0.35 + Math.min(Math.abs(v) * 0.09, 6) + stormBell * 3.2;
      gust += (target - gust) * (target > gust ? 0.25 : 0.04);

      var my = mouse.y + window.scrollY, mx = mouse.x;
      ctx.clearRect(0, 0, W, H);
      var light = 'rgba(244,226,190,1)', dark = 'rgba(120,78,38,1)';
      for (var k = 0; k < parts.length; k++) {
        var p = parts[k];
        if (mouse.active) {
          var dx = p.x - mx, dy = p.y - my, dd = dx * dx + dy * dy;
          if (dd < 16900) {                       // 130 px
            var dist = Math.sqrt(dd) || 1, f = (1 - dist / 130) * 1.6 * p.d;
            p.vx += (dx / dist) * f - (dy / dist) * f * 0.6;
            p.vy += (dy / dist) * f + (dx / dist) * f * 0.6;
          }
        }
        p.vx *= 0.93; p.vy *= 0.93;
        p.x += (gust * p.d * 1.5 + p.vx) * dt;
        p.y += (Math.sin(time * 2 + p.ph) * 0.18 + p.vy - v * p.d * 0.05 + 0.04) * dt;
        if (p.x > W + 8) { p.x = -8; p.y = Math.random() * H; }
        else if (p.x < -8) p.x = W + 8;
        if (p.y > H + 8) p.y = -8; else if (p.y < -8) p.y = H + 8;
      }
      var streak = Math.min(gust * 1.6, 14);
      for (var c = 0; c < 2; c++) {
        ctx.fillStyle = c ? dark : light;
        for (var n = 0; n < parts.length; n++) {
          var o = parts[n];
          if (o.dark !== (c === 1)) continue;
          ctx.globalAlpha = o.a;
          ctx.fillRect(o.x, o.y, o.r + streak * o.d, o.r);
        }
      }
      ctx.globalAlpha = 1;
      raf = requestAnimationFrame(frame);
    }
    function start() {
      if (running || !visible || document.hidden) return;
      running = true; last = 0; scrollVel = 0;
      raf = requestAnimationFrame(frame);
    }
    function stop() {
      running = false;
      if (raf) cancelAnimationFrame(raf);
      raf = 0;
    }
    if ('IntersectionObserver' in window) {
      new IntersectionObserver(function (en) {
        visible = en[0].isIntersecting;
        if (visible) start(); else stop();
      }).observe(world);
    }
    document.addEventListener('visibilitychange', function () { if (document.hidden) stop(); else start(); });
    resize();
    start();
    api.resize = resize; api.start = start; api.stop = stop;
    return api;
  })();

  /* ---------------- Cursor trail: a little dust behind the pointer ---------------- */
  var trail = (function () {
    var cv = $('.trail');
    if (!cv || reduce || !fineMq.matches || !cv.getContext) { if (cv) cv.style.display = 'none'; return null; }
    var ctx = cv.getContext('2d'), motes = [], raf = 0, W = 0, H = 0, dpr = 1, lx = 0, ly = 0;
    function size() {
      dpr = Math.min(window.devicePixelRatio || 1, 1.5);
      W = window.innerWidth; H = window.innerHeight;
      cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
    size();
    window.addEventListener('resize', size);
    function frame() {
      raf = 0;
      ctx.clearRect(0, 0, W, H);
      for (var i = motes.length - 1; i >= 0; i--) {
        var m = motes[i];
        m.life -= 0.032; m.x += m.vx; m.y += m.vy; m.vy += 0.02;
        if (m.life <= 0) { motes.splice(i, 1); continue; }
        ctx.globalAlpha = m.life * 0.55;
        ctx.fillStyle = m.c;
        ctx.fillRect(m.x, m.y, m.r, m.r);
      }
      ctx.globalAlpha = 1;
      if (motes.length) raf = requestAnimationFrame(frame);
    }
    return {
      add: function (x, y) {
        var dx = x - lx, dy = y - ly;
        lx = x; ly = y;
        if (dx * dx + dy * dy < 30) return;
        for (var k = 0; k < 2 && motes.length < 70; k++) {
          motes.push({
            x: x + 6 + Math.random() * 6, y: y + 10 + Math.random() * 6,
            vx: -dx * 0.02 + (Math.random() - 0.5) * 0.6, vy: (Math.random() - 0.7) * 0.5,
            r: 1 + Math.random() * 1.8, life: 0.7 + Math.random() * 0.3,
            c: Math.random() < 0.5 ? '#e9d3a4' : '#b98b52'
          });
        }
        if (!raf) raf = requestAnimationFrame(frame);
      }
    };
  })();

  window.addEventListener('pointermove', function (e) {
    if (e.pointerType && e.pointerType !== 'mouse') return;
    mouse.x = e.clientX; mouse.y = e.clientY; mouse.active = true;
    if (trail) trail.add(e.clientX, e.clientY);
  }, { passive: true });
  document.addEventListener('pointerleave', function () { mouse.active = false; });
  doc.addEventListener('mouseleave', function () { mouse.active = false; });

  reduceMq.addEventListener && reduceMq.addEventListener('change', function (e) {
    reduce = e.matches;
    if (reduce) {
      dust.stop();
      reveals.forEach(function (el) { el.classList.add('is-in'); });
      layers.forEach(function (l) { l.el.style.transform = ''; });
      copy.style.transform = ''; copy.style.opacity = '';
    }
  });

  /* ---------------- Releases: keep the version badges current ---------------- */
  var rel = null;
  function fmtDate(iso) {
    try {
      return new Date(iso).toLocaleDateString(lang === 'ru' ? 'ru-RU' : 'en-GB', { day: 'numeric', month: 'long', year: 'numeric' });
    } catch (e) { return iso.slice(0, 10); }
  }
  function renderReleases() {
    if (!rel) return;
    var sTag = $('[data-rel="stable"]'), pTag = $('[data-rel="pre"]');
    var sDate = $('[data-rel-date="stable"]'), pDate = $('[data-rel-date="pre"]');
    if (rel.stable) {
      sTag.textContent = rel.stable.tag_name;
      sDate.textContent = TEXT[lang].published + fmtDate(rel.stable.published_at);
    }
    if (rel.pre) {
      pTag.textContent = rel.pre.tag_name;
      pDate.textContent = TEXT[lang].published + fmtDate(rel.pre.published_at);
    } else if (rel.stable) {
      pTag.textContent = '—';
      pDate.textContent = TEXT[lang].noPre;
    }
  }
  function useReleases(list) {
    var stable = null, pre = null;
    list.forEach(function (r) {
      if (r.draft) return;
      if (!r.prerelease && (!stable || r.published_at > stable.published_at)) stable = r;
    });
    list.forEach(function (r) {
      if (r.draft || !r.prerelease) return;
      if (stable && r.published_at < stable.published_at) return;
      if (!pre || r.published_at > pre.published_at) pre = r;
    });
    rel = { stable: stable, pre: pre };
    renderReleases();
  }
  (function loadReleases() {
    if (!window.fetch) return;
    var cached = null;
    try { cached = JSON.parse(sessionStorage.getItem('kk-releases') || 'null'); } catch (e) {}
    if (cached && Date.now() - cached.t < 10 * 60 * 1000) { useReleases(cached.list); return; }
    fetch('https://api.github.com/repos/' + REPO + '/releases?per_page=30', { headers: { Accept: 'application/vnd.github+json' } })
      .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(function (data) {
        var list = data.map(function (r) {
          return { tag_name: r.tag_name, prerelease: r.prerelease, draft: r.draft, published_at: r.published_at || '' };
        });
        try { sessionStorage.setItem('kk-releases', JSON.stringify({ t: Date.now(), list: list })); } catch (e) {}
        useReleases(list);
      })
      .catch(function () { /* offline or rate-limited: the badges written in the page stay */ });
  })();

  /* ---------------- Start ---------------- */
  setLang(lang, false);
})();
