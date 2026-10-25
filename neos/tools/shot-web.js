// node tools/shot-web.js <url> <out.png>: a real browser (Chromium via Playwright) loads a page and saves a screenshot.
const { chromium } = require('/opt/node22/lib/node_modules/playwright');
(async () => {
  const b = await chromium.launch({ executablePath: process.env.CHROMIUM || '/opt/pw-browsers/chromium', args: ['--no-sandbox'] });
  const p = await b.newPage({ viewport: { width: 1280, height: 800 } });
  await p.goto(process.argv[2], { waitUntil: 'networkidle', timeout: 120000 });
  await p.waitForTimeout(2500);
  console.log('title:', await p.title());
  await p.screenshot({ path: process.argv[3] });
  await b.close();
})().catch(e => { console.error(e); process.exit(1); });
