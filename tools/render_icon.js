// Renders the app icon's PNGs from its SVG sources with the Playwright Chromium, then
// tools/make_ico.py packs them into the .ico the installer uses. The outputs are committed, so
// building Weft needs neither; rerun both after editing the SVGs:
//   node tools/render_icon.js && python3 tools/make_ico.py
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const dir = path.join(__dirname, '..', 'app', 'resources', 'icon');
// The detailed drawing from 48 px up; a simpler one (no grid or nodes) below that.
const jobs = [['weft-icon.svg', [256, 64, 48]], ['weft-icon-small.svg', [32, 24, 16]]];

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  for (const [src, sizes] of jobs) {
    const svg = fs.readFileSync(path.join(dir, src), 'utf8');
    for (const s of sizes) {
      await page.setViewportSize({ width: s, height: s });
      const sized = svg.replace(/width="\d+" height="\d+"/, `width="${s}" height="${s}"`);
      await page.setContent(`<html><body style="margin:0">${sized}</body></html>`);
      await page.screenshot({ path: path.join(dir, `weft-icon-${s}.png`), omitBackground: true });
    }
  }
  await browser.close();
})();
