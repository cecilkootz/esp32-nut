import { test, expect } from './fixtures';

test('Status indicators update correctly from API', async ({ page }) => {
  // Intercept the API call to mock the response
  await page.route('**/api/system-status', async route => {
    const json = {
      wifi: { status: 'HomeNetwork' },
      ups: { status: 'Eaton 3S' }
    };
    await route.fulfill({ json });
  });

  // Navigate to the dashboard
  await page.goto('/');

  // Check that the indicators are updated correctly after polling
  const wifiLabel = page.locator('#lbl-wifi');
  const upsLabel = page.locator('#lbl-ups');

  await expect(wifiLabel).toHaveText('Wi-Fi: HomeNetwork');
  await expect(upsLabel).toHaveText('UPS: Eaton 3S');
  
  const wifiIndicator = page.locator('#ind-wifi');
  await expect(wifiIndicator).toHaveClass(/success/);
  
  const upsIndicator = page.locator('#ind-ups');
  await expect(upsIndicator).toHaveClass(/success/);
});

test('Status is polled on load and every 3 s alongside the UPS poll', async ({ page }) => {
  let polls = 0;
  await page.route('**/api/system-status', async route => {
    polls++;
    await route.fulfill({ json: { wifi: { status: `Poll ${polls}` } } });
  });
  await page.route('**/api/ups-vars', async route => {
    await route.fulfill({ json: { 'ups.status': 'OL' } });
  });

  // Timers fire only when the test runs the clock
  await page.clock.install({ time: 0 });
  await page.clock.pauseAt(0);
  await page.goto('/');

  const wifiLabel = page.locator('#lbl-wifi');
  await expect(wifiLabel).toHaveText('Wi-Fi: Poll 1');

  // runFor fires timers back to back, so each status poll starts while a UPS request is in flight
  for (const poll of [2, 3]) {
    await page.clock.runFor(3000);
    await expect(wifiLabel).toHaveText(`Wi-Fi: Poll ${poll}`);
  }
});
