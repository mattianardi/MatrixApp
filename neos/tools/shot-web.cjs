// node tools/shot-web.cjs <url> <out.png>: a real browser (Chromium via Playwright) loads a page and saves a screenshot.
const { chromium } = require('/opt/node22/lib/node_modules/playwright');
(async () => {
  const b = await chromium.launch({ executablePath: process.env.CHROMIUM || '/opt/pw-browsers/chromium', args: ['--no-sandbox'] });
  const p = await b.newPage({ viewport: { width: 1280, height: 800 } });
  await p.route(/^https?:\/\/(?!127\.0\.0\.1)/, r => r.abort());             // external fonts/CDNs are unreachable here: fail fast instead of stalling the capture
  p.on('console', m => console.log('console:', m.text().slice(0, 160)));
  p.on('requestfailed', r => console.log('failed:', r.url()));
  try { await p.goto(process.argv[2], { waitUntil: 'domcontentloaded', timeout: 240000 }); } catch (e) { console.log('goto:', String(e).slice(0, 120)); }
  await p.waitForTimeout(25000);                                 // a single-page app keeps polling: never 'idle'
  console.log('title:', await p.title());
  await p.screenshot({ path: process.argv[3], timeout: 120000 });
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
