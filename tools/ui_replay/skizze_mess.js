// Misst die statischen Skizzen unter docs/ui-konzept/ (Playwright, headless
// Chromium): je Skizze und Fenstergröße Screenshots und die Lage jedes
// Bedienelements mit data-ctl — Reiter, scrollender Behälter, Versatz darin,
// im Bild ohne Scrollen, verdeckt (Toast an), Größe, Menü.
// Aufruf: NODE_PATH=~/dev/unspill/node_modules node tools/ui_replay/skizze_mess.js <out.json>
'use strict';
const path = require('path');
const fs = require('fs');
const { chromium } = require('playwright');

const DIR = path.resolve(__dirname, '../../docs/ui-konzept');
const SKETCHES = ['v0-heute', 'v1-reiter', 'v2-seitenleiste', 'v3-referenzspur'];
const SIZES = [[1920, 1080], [1366, 768]];

async function measure (page) {
  return page.evaluate(() => {
    const out = {};
    const scroller = (el) => {
      for (let a = el.parentElement; a && a !== document.body; a = a.parentElement) {
        const cs = getComputedStyle(a);
        if (/(auto|scroll)/.test(cs.overflowY) && a.scrollHeight > a.clientHeight + 1) return a;
      }
      return null;
    };
    const hidden = (el) => { for (let a = el; a; a = a.parentElement) { if (getComputedStyle(a).display === 'none') return true; } return false; };
    for (const el of document.querySelectorAll('[data-ctl]')) {
      const key = el.dataset.ctl;
      if (out[key]) continue;
      const menu = el.closest('.menu');
      if (menu && !menu.classList.contains('open')) { out[key] = { menu: menu.dataset.menu }; continue; }
      if (hidden(el)) continue;
      const r = el.getBoundingClientRect();
      const sc = scroller(el);
      let clip = { l: 0, t: 0, r: innerWidth, b: innerHeight };
      for (let a = el.parentElement; a && a !== document.body; a = a.parentElement) {
        const cs = getComputedStyle(a);
        if (/(auto|scroll|hidden|clip)/.test(cs.overflowX + ' ' + cs.overflowY)) {
          const ar = a.getBoundingClientRect();
          clip = { l: Math.max(clip.l, ar.left), t: Math.max(clip.t, ar.top), r: Math.min(clip.r, ar.right), b: Math.min(clip.b, ar.bottom) };
        }
      }
      const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
      const inView = cx >= clip.l && cx <= clip.r - 1 && cy >= clip.t && cy <= clip.b - 1;
      let obscured = false;
      if (inView) { const top = document.elementFromPoint(cx, cy); obscured = !!top && !(el === top || el.contains(top) || top.contains(el)); }
      const pane = el.closest('.tabpane');
      const o = { tab: pane ? pane.dataset.tab : null, x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height), inView, obscured };
      if (sc) {
        const cr = sc.getBoundingClientRect();
        o.scroller = sc.id || sc.className || sc.tagName;
        o.y0 = Math.round(r.top - cr.top + sc.scrollTop); o.y1 = Math.round(o.y0 + r.height);
        o.ch = sc.clientHeight; o.sh = sc.scrollHeight;
      }
      out[key] = o;
    }
    return out;
  });
}

(async () => {
  const outFile = process.argv[2] || 'mess.json';
  const browser = await chromium.launch();
  const res = {};
  fs.mkdirSync(path.join(DIR, 'shots'), { recursive: true });
  for (const sk of SKETCHES) {
    res[sk] = {};
    for (const [w, h] of SIZES) {
      const size = w + 'x' + h;
      const page = await browser.newPage({ viewport: { width: w, height: h } });
      await page.goto('file://' + path.join(DIR, sk + '.html') + '?toast=1&card=1');
      await page.waitForTimeout(300);
      await page.screenshot({ path: path.join(DIR, 'shots', `${sk}-${size}.png`) });
      const tabs = await page.$$eval('button.tab', bs => bs.map(b => b.dataset.tabbtn));
      const all = {};
      for (const t of (tabs.length ? tabs : [null])) {
        if (t) await page.click(`button.tab[data-tabbtn="${t}"]`);
        if (t === 'routing' || !t) {
          const slot = await page.$('[data-ctl="route.chain"]');
          if (slot) await slot.click();
        }
        await page.evaluate(() => { const x = document.querySelector('#toast'); x.innerHTML = '"Saw lead" applied in node #13'; x.classList.remove('hide'); });
        await page.waitForTimeout(80);
        const m = await measure(page);
        for (const [k, v] of Object.entries(m)) {
          // je Schlüssel: die Messung aus seinem eigenen Reiter (oder ohne Reiter)
          if (!all[k] || (v.tab && v.tab === t) || (all[k].menu && !v.menu)) all[k] = v;
        }
        if (t === 'reference') await page.screenshot({ path: path.join(DIR, 'shots', `${sk}-reference-${size}.png`) });
      }
      res[sk][size] = all;
      await page.close();
    }
  }
  await browser.close();
  fs.writeFileSync(outFile, JSON.stringify(res, null, 1));
  console.log('written', outFile);
})();
