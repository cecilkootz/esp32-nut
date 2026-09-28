import { test, expect } from './fixtures';

test.describe('Wi-Fi Configuration UI Debug', () => {
  test.beforeEach(async ({ page }) => {
    page.on('console', msg => console.log('BROWSER LOG:', msg.text()));
    page.on('pageerror', err => console.log('BROWSER ERROR:', err.message));
  });

  test('renders the main interface correctly', async ({ page }) => {
    await page.goto('/');
    await page.click('button[data-target="wifi"]');
    await expect(page.locator('h1')).toHaveText('Wi-Fi Config');
  });
});
