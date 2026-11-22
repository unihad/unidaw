// Messung der Bedienung im Flow-UI: jede Aktion der Treiber läuft über
// diese Klasse — Klicks, Doppelklicks, Ziehen, Tasten, nötiges Scrollen
// (als Mausrad-Rasten à 100 px), Mausweg, Ansichtswechsel, Hover-only,
// kleine Klickziele (< 24 px), verdeckte Ziele, Dateidialoge, Prompts.
// Gemessen wird am echten DOM (Bounding-Boxes, berechnete Styles); die
// Maus fährt wirklich (Playwright), das UI reagiert wie bei einem Menschen.

const WHEEL_PX = 100;   // eine Mausrad-Raste

class Meter {
  constructor (page, viewport) {
    this.page = page;
    this.vp = viewport;
    this.pos = { x: viewport.width / 2, y: viewport.height / 2 };
    this.rec = null;
    this.nextPrompts = [];
    page.on('dialog', async (d) => {
      const r = this.rec;
      if (d.type() === 'prompt') {
        const text = this.nextPrompts.length ? this.nextPrompts.shift() : null;
        if (r) {
          r.ui.prompts++;
          if (text !== null) { r.ui.keys += String(text).length + 1; r.ui.typed.push(String(text)); }
        }
        if (text === null) await d.dismiss(); else await d.accept(String(text));
      } else {
        if (r) r.ui.notes.push('dialog ' + d.type() + ': ' + d.message().slice(0, 80));
        await d.accept();
      }
    });
  }

  start (rec) {
    this.rec = rec;
    rec.ui = { clicks: 0, dblclicks: 0, drags: 0, keys: 0, wheel: 0, scrollPx: 0, scrollActs: 0,
               mousePx: 0, views: 0, viewList: [], hoverOnly: [], small: [], obscured: [],
               fileDialogs: 0, prompts: 0, typed: [], elems: [], notes: [], offscreenDrag: 0, keysFree: 0 };
  }

  async resolve (target) {
    if (!target) return null;
    if (typeof target === 'string') {
      const loc = this.page.locator(target).first();
      if (!(await loc.count())) return null;
      return loc.elementHandle();
    }
    if (typeof target === 'function') return target();
    return target;
  }

  // Lage, Sichtbarkeit und Ausschnitt (Viewport ∩ scrollende Vorfahren)
  async geometry (h) {
    return h.evaluate((el) => {
      const r = el.getBoundingClientRect();
      let hidden = '';
      for (let a = el; a && a !== document.documentElement; a = a.parentElement) {
        const cs = getComputedStyle(a);
        if (cs.display === 'none') { hidden = 'display'; break; }
        if (cs.visibility === 'hidden') { hidden = 'visibility'; break; }
        if (parseFloat(cs.opacity) < 0.05) { hidden = 'opacity'; break; }
      }
      if (!hidden && getComputedStyle(el).pointerEvents === 'none') hidden = 'pointer-events';
      let clip = { l: 0, t: 0, r: innerWidth, b: innerHeight };
      for (let a = el.parentElement; a && a !== document.body; a = a.parentElement) {
        const cs = getComputedStyle(a);
        if (/(auto|scroll|hidden|clip)/.test(cs.overflowX + ' ' + cs.overflowY)) {
          const ar = a.getBoundingClientRect();
          clip = { l: Math.max(clip.l, ar.left), t: Math.max(clip.t, ar.top),
                   r: Math.min(clip.r, ar.right), b: Math.min(clip.b, ar.bottom) };
        }
      }
      // Lanes der Timeline: die Kopfspalte links klebt (sticky) und deckt ab
      if (el.closest('.ltlane')) {
        const lab = document.querySelector('#linearRulerRow .ltlabel');
        if (lab) clip.l = Math.max(clip.l, lab.getBoundingClientRect().right);
      }
      return { x: r.left, y: r.top, w: r.width, h: r.height, hidden, clip,
               connected: el.isConnected };
    });
  }

  // Punkt in einem Element: at = {x, y} relativ zur Box, sonst die Mitte
  // des sichtbaren Teils (ein langer Clip wird dort geklickt, wo man ihn sieht)
  pointIn (g, at) {
    if (at) return { x: g.x + at.x, y: g.y + at.y };
    const l = Math.max(g.x, g.clip.l), r = Math.min(g.x + g.w, g.clip.r);
    const t = Math.max(g.y, g.clip.t), b = Math.min(g.y + g.h, g.clip.b);
    if (r - l >= 8 && b - t >= 6) return { x: (l + r) / 2, y: (t + b) / 2 };
    return { x: g.x + g.w / 2, y: g.y + g.h / 2 };
  }

  inClip (p, c) { return p.x >= c.l && p.x <= c.r - 1 && p.y >= c.t && p.y <= c.b - 1; }

  // Element bedienbar machen: Hover-Eltern anfahren, Scrollen zählen
  async prepare (target, opts = {}) {
    const r = this.rec;
    const desc = opts.desc || String(target);
    let h = await this.resolve(target);
    if (!h) throw new Error('element not found: ' + desc);
    let g = await this.geometry(h);

    if (g.hidden && opts.hover) {
      // Hover-Ziel erst ins Bild holen (Scrollen zählt), dann anfahren
      const hp = await this.prepare(opts.hover, { desc: 'hover target of ' + desc, noSize: true, at: opts.hoverAt });
      await this.moveTo(hp.p);
      await this.page.waitForTimeout(260);
      r.ui.hoverOnly.push(desc);
      h = await this.resolve(target);
      g = await this.geometry(h);
    }
    if (g.hidden && process.env.DEBUG_SHOTS) await this.page.screenshot({ path: process.env.DEBUG_SHOTS + '/hidden-' + r.seq + '.png' });
    if (g.hidden) throw new Error('element hidden (' + g.hidden + '): ' + desc);

    // Timeline: weit weg (mehr als 1,5 Bildbreiten)? Erst herauszoomen wie
    // eine Hand (− im Lineal-Kopf), dann scrollen. Nur ohne feste Klickstelle.
    if (!opts.at && !opts.noZoom) {
      for (let i = 0; i < 8; i++) {
        const far = await h.evaluate((el) => {
          if (!el.closest('#linearInner')) return null;
          const sc = document.querySelector('#linearScroll').getBoundingClientRect();
          const lab = document.querySelector('#linearRulerRow .ltlabel').getBoundingClientRect().right;
          const r = el.getBoundingClientRect();
          const x1 = Math.min(sc.right, innerWidth), w = x1 - lab;
          const d = r.right < lab ? lab - r.right : r.left > x1 ? r.left - x1 : 0;
          return { d, w, pps };
        });
        if (!far || far.d < 1.5 * far.w || far.pps <= 20) break;
        await this.click('#linearRulerRow .ltlabel button', { desc: 'zoom out −', noZoom: true, wait: 150 });
        h = await this.resolve(target);
        g = await this.geometry(h);
      }
    }

    let p = this.pointIn(g, opts.at);
    if (!this.inClip(p, g.clip) || (opts.whole && (g.y < g.clip.t || g.y + g.h > g.clip.b))) {
      const moved = await h.evaluate((el, [at, whole]) => {
        // Innerste scrollende Vorfahren zuerst, dann die Seite
        let dx = 0, dy = 0;
        const r0 = el.getBoundingClientRect();
        let px = at ? r0.left + at.x : r0.left + r0.width / 2;
        let py = at ? r0.top + at.y : r0.top + r0.height / 2;
        for (let a = el.parentElement; a; a = a.parentElement) {
          const sc = a === document.body ? document.scrollingElement : a;
          const cs = getComputedStyle(a);
          const scrollY = a === document.body || /(auto|scroll)/.test(cs.overflowY);
          const scrollX = a === document.body || /(auto|scroll)/.test(cs.overflowX);
          if (!scrollX && !scrollY) continue;
          const ar = a === document.body ? { left: 0, top: 0, right: innerWidth, bottom: innerHeight }
                                         : a.getBoundingClientRect();
          // Sticky Kopfspalte der Timeline: Lanes beginnen rechts davon
          let left = ar.left;
          if (a.id === 'linearScroll' && el.closest('.ltlane')) {
            const lab = document.querySelector('#linearRulerRow .ltlabel');
            if (lab) left = Math.max(left, lab.getBoundingClientRect().right);
          }
          // Piano-Roll: die Tastenspalte klebt links
          if (a.id === 'prWrap') left = Math.max(left, document.querySelector('#prKeys').getBoundingClientRect().right);
          const right = Math.min(ar.right, innerWidth), bottom = Math.min(ar.bottom, innerHeight);
          const top = Math.max(ar.top, 0);
          if (scrollY && a.scrollHeight > a.clientHeight + 1) {
            let want = 0;
            const h2 = whole ? r0.height : 0;
            if (py - h2 / 2 < top + 4) want = py - h2 / 2 - (top + 40);
            else if (py + h2 / 2 > bottom - 4) want = py + h2 / 2 - (bottom - 40);
            if (want) { const b0 = sc.scrollTop; sc.scrollTop += want; const d = sc.scrollTop - b0; dy += Math.abs(d); py -= d; }
          }
          if (scrollX && a.scrollWidth > a.clientWidth + 1) {
            let want = 0;
            if (px < left + 4) want = px - (left + 80);
            else if (px > right - 4) want = px - (right - 80);
            if (want) { const b0 = sc.scrollLeft; sc.scrollLeft += want; const d = sc.scrollLeft - b0; dx += Math.abs(d); px -= d; }
          }
        }
        return { dx, dy };
      }, [opts.at || null, !!opts.whole]);
      const notches = Math.ceil(moved.dx / WHEEL_PX) + Math.ceil(moved.dy / WHEEL_PX);
      if (notches > 0) {
        r.ui.wheel += notches;
        r.ui.scrollPx += Math.round(moved.dx + moved.dy);
        r.ui.scrollActs++;
      }
      await this.page.waitForTimeout(60);
      h = await this.resolve(target);
      g = await this.geometry(h);
      p = this.pointIn(g, opts.at);
      if (!this.inClip(p, g.clip)) r.ui.notes.push('not reachable by scrolling: ' + desc);
    }

    // Kleine Ziele (WCAG 2.5.8: 24 × 24 px) — Regler und Lanes ausgenommen
    if (!opts.noSize && (g.w < 24 || g.h < 24) && !r.ui.small.some(s => s.desc === desc))
      r.ui.small.push({ desc, w: Math.round(g.w), h: Math.round(g.h) });

    // Verdeckt? Was liegt unter dem Punkt?
    const ok = await h.evaluate((el, pt) => {
      const top = document.elementFromPoint(pt.x, pt.y);
      return !top || el === top || el.contains(top) || top.contains(el);
    }, p);
    if (!ok) r.ui.obscured.push(desc);

    r.ui.elems.push({ desc, x: Math.round(g.x), y: Math.round(g.y), w: Math.round(g.w), h: Math.round(g.h) });
    return { h, p, g };
  }

  async moveTo (p) {
    const d = Math.hypot(p.x - this.pos.x, p.y - this.pos.y);
    this.rec.ui.mousePx += d;
    await this.page.mouse.move(p.x, p.y, { steps: Math.max(1, Math.min(8, Math.round(d / 120))) });
    this.pos = { x: p.x, y: p.y };
  }

  async hover (target, opts = {}) {
    const h = await this.resolve(target);
    if (!h) throw new Error('hover target not found: ' + (opts.desc || target));
    const g = await this.geometry(h);
    await this.moveTo(this.pointIn(g, opts.at));
    await this.page.waitForTimeout(260);   // Einblenden (CSS-Transition)
  }

  async click (target, opts = {}) {
    const { p } = await this.prepare(target, opts);
    await this.moveTo(p);
    const mods = opts.modifiers || [];
    for (const k of mods) { await this.page.keyboard.down(k); this.rec.ui.keys++; }
    if (process.env.DEBUG_SHOTS && process.env.DEBUG_SEQ && process.env.DEBUG_SEQ.split(',').map(Number).includes(this.rec.seq))
      await this.page.screenshot({ path: process.env.DEBUG_SHOTS + '/click-' + this.rec.seq + '-' + (this.rec.ui.clicks + this.rec.ui.dblclicks) + '.png' });
    if (opts.dbl) { await this.page.mouse.dblclick(p.x, p.y); this.rec.ui.dblclicks++; }
    else { await this.page.mouse.click(p.x, p.y); this.rec.ui.clicks++; }
    for (const k of mods) await this.page.keyboard.up(k);
    if (opts.view) this.view(opts.view);
    await this.page.waitForTimeout(opts.wait ?? 80);
    return p;
  }

  // Ziehen: von einem Punkt im Element zu {x, y} (Bildschirm) oder um dx/dy
  async drag (target, opts = {}) {
    const { p } = await this.prepare(target, opts);
    const to = opts.to || { x: p.x + (opts.dx || 0), y: p.y + (opts.dy || 0) };
    await this.moveTo(p);
    const mods = opts.modifiers || [];
    for (const k of mods) { await this.page.keyboard.down(k); this.rec.ui.keys++; }
    await this.page.mouse.down();
    const steps = Math.max(4, Math.min(30, Math.round(Math.hypot(to.x - p.x, to.y - p.y) / 20)));
    await this.page.mouse.move(to.x, to.y, { steps });
    if (process.env.DEBUG_SHOTS && (!process.env.DEBUG_SEQ || process.env.DEBUG_SEQ.split(',').map(Number).includes(this.rec.seq))) await this.page.screenshot({ path: process.env.DEBUG_SHOTS + '/drag-' + this.rec.seq + '-' + (this.rec.ui.drags) + '.png' });
    await this.page.mouse.up();
    for (const k of mods) await this.page.keyboard.up(k);
    this.rec.ui.drags++;
    this.rec.ui.mousePx += Math.hypot(to.x - p.x, to.y - p.y);
    if (to.x < 0 || to.y < 0 || to.x > this.vp.width || to.y > this.vp.height) {
      this.rec.ui.offscreenDrag++;
      this.rec.ui.notes.push('drag ends outside the window: ' + (opts.desc || ''));
    }
    this.pos = { x: to.x, y: to.y };
    await this.page.waitForTimeout(opts.wait ?? 120);
    return { from: p, to };
  }

  async type (text, opts = {}) {
    text = String(text);
    await this.page.keyboard.type(text);
    this.rec.ui.keys += text.length;
    if (opts.free) this.rec.ui.keysFree += text.length;
    else this.rec.ui.typed.push(text);
  }

  async press (combo) {
    await this.page.keyboard.press(combo);
    this.rec.ui.keys += combo.split('+').length;
  }

  // Eingabefeld leeren und neu füllen (Klick, Strg+A, tippen)
  async fill (target, text, opts = {}) {
    await this.click(target, opts);
    await this.press('Control+a');
    await this.type(text, opts);
  }

  async select (target, pick, opts = {}) {
    await this.click(target, opts);
    const h = await this.resolve(target);
    const n = await h.evaluate(el => el.options.length);
    await h.selectOption(pick);
    this.rec.ui.clicks++;
    if (n > 25) this.rec.ui.notes.push('long list (' + n + ' entries): ' + (opts.desc || ''));
    await this.page.waitForTimeout(opts.wait ?? 120);
  }

  // Scroll-Container auf eine Position bringen (Mausrad, gezählt)
  async scrollTo (sel, left, top) {
    const d = await this.page.evaluate(([s, l, t]) => {
      const e = document.querySelector(s);
      const l0 = e.scrollLeft, t0 = e.scrollTop;
      if (l !== null && l !== undefined) e.scrollLeft = l;
      if (t !== null && t !== undefined) e.scrollTop = t;
      return { dx: Math.abs(e.scrollLeft - l0), dy: Math.abs(e.scrollTop - t0) };
    }, [sel, left ?? null, top ?? null]);
    const n = Math.ceil(d.dx / WHEEL_PX) + Math.ceil(d.dy / WHEEL_PX);
    if (n) { this.rec.ui.wheel += n; this.rec.ui.scrollPx += Math.round(d.dx + d.dy); this.rec.ui.scrollActs++; }
    await this.page.waitForTimeout(60);
  }

  view (desc) {
    this.rec.ui.views++;
    this.rec.ui.viewList.push(desc);
  }

  prompt (text) { this.nextPrompts.push(text); }

  // Dateidialog des Betriebssystems: zählt extra (außerhalb des Browsers)
  async fileDialog (trigger, file, opts = {}) {
    const [chooser] = await Promise.all([
      this.page.waitForEvent('filechooser', { timeout: 5000 }),
      this.click(trigger, opts),
    ]);
    const up = this.page.waitForRequest(r => r.url().includes('/upload'), { timeout: 8000 }).catch(() => null);
    await chooser.setFiles(file);
    this.rec.ui.fileDialogs++;
    await up;
    await this.page.waitForTimeout(200);
  }
}

module.exports = { Meter };
