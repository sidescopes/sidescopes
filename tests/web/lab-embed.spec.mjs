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

async function openSettledLab(page) {
  await page.addInitScript(() => {
    if (!window.localStorage.getItem('sidescopes.lab.preferences.v1')) {
      window.localStorage.setItem('sidescopes.lab.preferences.v1', 'tour_settled=1\n');
    }
  });
  await page.goto('http://127.0.0.1:8099/index.html');
  await expect(page.locator('#credit')).toContainText('Public domain.', { timeout: 20_000 });
}

test('the loader and WebAssembly use the same nonempty build digest', async ({ page }) => {
  const wasmRequested = page.waitForRequest((request) => new URL(request.url()).pathname.endsWith('.wasm'));
  await page.goto('http://127.0.0.1:8099/index.html');
  const request = await wasmRequested;
  const loader = await page.locator('script[src^="sidescopes-lab.js"]').getAttribute('src');
  const digest = new URL(loader, page.url()).searchParams.get('b');
  expect(digest).toMatch(/^[a-f0-9]{10}$/);
  expect(new URL(request.url()).searchParams.get('b')).toBe(digest);
});

test('missing photographs still produce a usable embedded Lab and ready signal', async ({ page }) => {
  const errors = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.route('**/samples/*.jpg', (route) => route.fulfill({ status: 404, body: '' }));
  await page.goto('http://127.0.0.1:4173/embed.html');
  await expect(page.getByTitle('SideScopes Lab')).toHaveAttribute('data-ready', 'true', { timeout: 20_000 });
  await expect(page.frameLocator('iframe').locator('#credit')).toContainText('Generated color bars');
  expect(await page.evaluate(() => window.scrollY)).toBe(0);
  expect(errors).toEqual([]);
});

test('the last selected sample wins when an earlier download finishes late', async ({ page }) => {
  await page.addInitScript(() => {
    const decode = window.createImageBitmap.bind(window);
    window.__decodedImages = 0;
    window.createImageBitmap = async (...args) => {
      const result = await decode(...args);
      ++window.__decodedImages;
      return result;
    };
  });
  await openSettledLab(page);
  let release;
  const pending = new Promise((resolve) => { release = resolve; });
  let requested;
  const began = new Promise((resolve) => { requested = resolve; });
  await page.route('**/samples/neutral-detail.jpg', async (route) => {
    requested();
    await pending;
    await route.fulfill({ status: 200, contentType: 'image/jpeg', body: SAMPLE_PIXEL });
  });
  await page.getByRole('button', { name: 'Near-neutral, fine detail' }).click();
  await began;
  const newer = page.getByRole('button', { name: 'Deep shadow beside clipped highlight' });
  await newer.click();
  await expect(page.locator('#credit')).toContainText('NASA');
  const lateResponse = page.waitForResponse('**/samples/neutral-detail.jpg');
  release();
  await lateResponse;
  await expect.poll(() => page.evaluate(() => window.__decodedImages)).toBe(3);
  await expect(newer).toHaveAttribute('aria-current', 'true');
  await expect(page.locator('#credit')).toContainText('NASA');
});

test('pinning can be cancelled, rearmed with the same swatch, and replaced by drawing', async ({ page }) => {
  await openSettledLab(page);
  const canvas = page.locator('#canvas');
  await canvas.focus();
  await page.keyboard.press('p');
  await expect(canvas).toHaveCSS('cursor', /url\(/);
  await page.keyboard.press('Escape');
  await expect(canvas).not.toHaveCSS('cursor', /crosshair/);
  await page.keyboard.press('p');
  await expect(canvas).toHaveCSS('cursor', /url\(/);
  await page.keyboard.press('d');
  await expect(canvas).toHaveCSS('cursor', 'crosshair');
  await page.keyboard.press('p');
  await expect(canvas).toHaveCSS('cursor', /url\(/);
  await page.keyboard.press('Escape');
  await expect(canvas).not.toHaveCSS('cursor', /crosshair/);
});

test('transparent local images are composited over the measured black desktop', async ({ page }) => {
  await page.route('**/sidescopes-lab.js?*', async (route) => {
    const response = await route.fetch();
    await route.fulfill({ response, body: `${await response.text()}\n
      const originalLabFactory = SideScopesLab;
      SideScopesLab = async (options) => {
        const instance = await originalLabFactory(options);
        const call = instance.ccall.bind(instance);
        let pointer = 0;
        instance.ccall = (name, ...args) => {
          if (name === 'labFrameReady') {
            window.__lastSubmittedPixel = Array.from(instance.HEAPU8.slice(pointer, pointer + 4));
          }
          const result = call(name, ...args);
          if (name === 'labFrameBuffer') { pointer = result; }
          return result;
        };
        return instance;
      };
    ` });
  });
  await openSettledLab(page);
  const png = await page.evaluate(() => {
    const canvas = document.createElement('canvas');
    canvas.width = 1; canvas.height = 1;
    const pen = canvas.getContext('2d');
    pen.fillStyle = 'rgba(255, 0, 0, 0.5)';
    pen.fillRect(0, 0, 1, 1);
    return canvas.toDataURL('image/png').split(',')[1];
  });
  await page.locator('#file').setInputFiles({
    name: 'transparent.png', mimeType: 'image/png', buffer: Buffer.from(png, 'base64'),
  });
  await expect(page.locator('#credit')).toContainText('Your image.');
  expect(await page.evaluate(() => window.__lastSubmittedPixel)).toEqual([128, 0, 0, 255]);
});

async function pressLabKey(page, key) {
  // Let the immediate-mode UI consume both transitions before repeating a
  // key. A down/up/down burst inside one animation frame is not a gesture.
  const frame = () => page.evaluate(() => new Promise((resolve) => {
    requestAnimationFrame(() => requestAnimationFrame(resolve));
  }));
  await page.keyboard.down(key);
  await frame();
  await page.keyboard.up(key);
  await frame();
}

test('zoom and saved scope presets survive a browser reload', async ({ page }) => {
  await openSettledLab(page);
  const canvas = page.locator('#canvas');
  const saved = () => page.evaluate(() => window.localStorage.getItem('sidescopes.lab.preferences.v1'));
  await canvas.focus();
  await pressLabKey(page, 'v');
  await expect.poll(saved).toContain('scope_stack=[org.sidescopes.vectorscope]\n');
  await pressLabKey(page, 'z');
  await expect.poll(saved).toContain('vectorscope_zoom=2\n');
  await page.keyboard.down('Shift');
  await pressLabKey(page, '2');
  await expect.poll(saved).toContain('layout.preset2.stack=[org.sidescopes.vectorscope]\n');
  await page.keyboard.up('Shift');
  await pressLabKey(page, 'w');
  await expect.poll(saved).toContain('scope_stack=[org.sidescopes.waveform]\n');
  await pressLabKey(page, '2');
  await expect.poll(saved).toContain('scope_stack=[org.sidescopes.vectorscope]\n');
  await page.reload();
  await expect(page.locator('#credit')).toContainText('Public domain.', { timeout: 20_000 });
  await canvas.focus();
  // The restored zoom is 2, so its next step is 4. This reads the restored
  // application state rather than merely checking the saved text exists.
  await pressLabKey(page, 'z');
  await expect.poll(saved).toContain('vectorscope_zoom=4\n');
  await pressLabKey(page, 'w');
  await pressLabKey(page, '2');
  await expect.poll(saved).toContain('scope_stack=[org.sidescopes.vectorscope]\n');
});
