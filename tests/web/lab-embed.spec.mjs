import { expect, test } from '@playwright/test';

const SAMPLE_PIXEL = Buffer.from(
  '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgICAgMCAgIDAwMDBAYEBAQEBAgGBgUGCQgKCgkICQkKDA8MCgsOCwkJDRENDg8QEBEQCgwSExIQEw8QEBD/wAALCAAQABABAREA/8QAFAABAAAAAAAAAAAAAAAAAAAAAP/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AAP/Z',
  'base64',
);

test.beforeEach(async ({ page }) => {
  await page.route('**/samples/*.jpg', (route) => route.fulfill({
    status: 200,
    contentType: 'image/jpeg',
    body: SAMPLE_PIXEL,
  }));
});

async function touchDrag(page, from, to) {
  const session = await page.context().newCDPSession(page);
  await session.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [{ x: from.x, y: from.y }],
  });
  for (let step = 1; step <= 5; ++step) {
    await session.send('Input.dispatchTouchEvent', {
      type: 'touchMove',
      touchPoints: [{
        x: from.x + (to.x - from.x) * step / 5,
        y: from.y + (to.y - from.y) * step / 5,
      }],
    });
  }
  await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
}

test('an embedded mobile Lab keeps one scrollbar and owns interactive gestures', async ({ page }) => {
  page.on('pageerror', (error) => console.error(`Lab page error: ${error.message}`));
  await page.goto('http://127.0.0.1:4173/embed.html');
  const frameElement = page.getByTitle('SideScopes Lab');
  await expect(frameElement).toHaveAttribute('data-ready', 'true', { timeout: 20_000 });
  await expect(frameElement).toHaveAttribute('scrolling', 'no');
  expect(await page.evaluate(() => window.scrollY)).toBe(0);
  const lab = page.frameLocator('iframe');

  const stageBox = await lab.locator('#stage').boundingBox();
  expect(stageBox).not.toBeNull();
  expect(stageBox.height).toBeGreaterThanOrEqual(790);

  const flatField = lab.getByRole('button', { name: 'Near-neutral, fine detail' });
  await flatField.scrollIntoViewIfNeeded();
  const flatFieldBox = await flatField.boundingBox();
  expect(flatFieldBox).not.toBeNull();
  await page.touchscreen.tap(flatFieldBox.x + flatFieldBox.width / 2, flatFieldBox.y + flatFieldBox.height / 2);
  await expect(lab.locator('#credit')).toContainText('National Park Service');

  const exposure = lab.getByRole('slider', { name: 'Exposure' });
  await exposure.evaluate((slider) => { slider.value = '0'; slider.dispatchEvent(new Event('input')); });
  await exposure.scrollIntoViewIfNeeded();
  const exposureBox = await exposure.boundingBox();
  expect(exposureBox).not.toBeNull();
  const beforeSlider = await page.evaluate(() => window.scrollY);
  await touchDrag(
    page,
    { x: exposureBox.x + exposureBox.width / 2, y: exposureBox.y + exposureBox.height / 2 },
    { x: exposureBox.x + exposureBox.width * 0.85, y: exposureBox.y + exposureBox.height / 2 },
  );
  await expect(exposure).not.toHaveValue('0');
  expect(await page.evaluate(() => window.scrollY)).toBe(beforeSlider);

  const canvas = lab.locator('#canvas');
  await canvas.evaluate((element) => {
    window.__gestureEvents = [];
    for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel']) {
      element.addEventListener(type, () => window.__gestureEvents.push(type));
    }
  });
  await canvas.scrollIntoViewIfNeeded();
  const canvasBox = await canvas.boundingBox();
  expect(canvasBox).not.toBeNull();
  const beforeCanvas = await page.evaluate(() => window.scrollY);
  await touchDrag(
    page,
    { x: canvasBox.x + canvasBox.width * 0.25, y: canvasBox.y + canvasBox.height * 0.35 },
    { x: canvasBox.x + canvasBox.width * 0.38, y: canvasBox.y + canvasBox.height * 0.47 },
  );
  const events = await canvas.evaluate(() => window.__gestureEvents);
  expect(events[0]).toBe('pointerdown');
  expect(events).toContain('pointermove');
  expect(events.at(-1)).toBe('pointerup');
  expect(events).not.toContain('pointercancel');
  expect(await page.evaluate(() => window.scrollY)).toBe(beforeCanvas);

  const credit = lab.locator('#credit');
  await credit.scrollIntoViewIfNeeded();
  const creditBox = await credit.boundingBox();
  expect(creditBox).not.toBeNull();
  const beforeCredit = await page.evaluate(() => window.scrollY);
  await touchDrag(
    page,
    { x: creditBox.x + creditBox.width / 2, y: creditBox.y + creditBox.height / 2 },
    { x: creditBox.x + creditBox.width / 2, y: creditBox.y - 140 },
  );
  expect(await page.evaluate(() => window.scrollY)).toBeGreaterThan(beforeCredit);
});

test('a standalone Lab takes keyboard focus at startup', async ({ page }) => {
  await page.goto('http://127.0.0.1:8099/index.html');
  await expect.poll(
    () => page.evaluate(() => document.activeElement?.id),
    { timeout: 20_000 },
  ).toBe('canvas');
});
