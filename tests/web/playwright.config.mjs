import { defineConfig } from '@playwright/test';
import { fileURLToPath } from 'node:url';

const labDirectory = process.env.SIDESCOPES_LAB_DIR || 'build-web';
const repository = fileURLToPath(new URL('../..', import.meta.url));

export default defineConfig({
  testDir: '.',
  testMatch: 'lab-embed.spec.mjs',
  timeout: 30_000,
  forbidOnly: Boolean(process.env.CI),
  retries: process.env.CI ? 1 : 0,
  reporter: 'line',
  use: {
    browserName: 'chromium',
    viewport: { width: 390, height: 844 },
    deviceScaleFactor: 1,
    hasTouch: true,
    isMobile: true,
  },
  webServer: [
    {
      command: `python3 -m http.server 8099 --bind 127.0.0.1 --directory ${labDirectory}`,
      url: 'http://127.0.0.1:8099/index.html',
      cwd: repository,
      reuseExistingServer: !process.env.CI,
    },
    {
      command: 'python3 -m http.server 4173 --bind 127.0.0.1 --directory tests/web',
      url: 'http://127.0.0.1:4173/embed.html',
      cwd: repository,
      reuseExistingServer: !process.env.CI,
    },
  ],
});
