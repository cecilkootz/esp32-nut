import { test, expect } from './fixtures';

test.describe('System Logs View', () => {
  test('should display mocked system logs in the terminal', async ({ page }) => {
    // Intercept the API call to /api/logs
    await page.route('**/api/logs', async (route) => {
      if (route.request().method() === 'GET') {
        await route.fulfill({
          status: 200,
          contentType: 'application/json',
          body: JSON.stringify([
            { id: 1, time: 1000, level: 'WARN', msg: 'Mocked warning message' }
          ])
        });
      } else {
        await route.fallback();
      }
    });

    // Timers fire only when the test runs the clock, so auto-refresh can't load the logs
    await page.clock.install({ time: 0 });
    await page.clock.pauseAt(0);

    // 1. Navigate to /
    await page.goto('/');

    // 2. Click on the "System Logs" tab
    await page.click('[data-target="logs"]');

    // 3. Wait for the terminal to update
    const terminalOutput = page.locator('#terminal-output');
    
    // Wait for the specific message to appear in the terminal
    await expect(terminalOutput).toContainText('Mocked warning message');

    // Verify the correct class is applied to the log line (e.g., span containing the log)
    const logLine = page.locator('.terminal-line', { hasText: 'Mocked warning message' });
    await expect(logLine.locator('.terminal-level-warn')).toBeVisible();
  });

  test('should append new logs on each auto-refresh', async ({ page }) => {
    const logs = [{ id: 1, time: 1000, level: 'INFO', msg: 'First message' }];
    await page.route('**/api/logs', route => route.fulfill({ json: logs }));

    await page.clock.install({ time: 0 });
    await page.clock.pauseAt(0);
    await page.goto('/');
    await page.click('[data-target="logs"]');

    const terminalOutput = page.locator('#terminal-output');
    await expect(terminalOutput).toContainText('First message');

    logs.push({ id: 2, time: 3000, level: 'INFO', msg: 'Second message' });
    await page.clock.runFor(2000);
    await expect(terminalOutput).toContainText('Second message');
    await expect(page.locator('.terminal-line', { hasText: 'First message' })).toHaveCount(1);
  });

  test('should pause and resume auto-refresh', async ({ page }) => {
    // 1. Navigate to /
    await page.goto('/');

    // 2. Click on the "System Logs" tab
    await page.click('[data-target="logs"]');

    const toggleAutoRefresh = page.locator('#toggle-auto-refresh');
    const terminalOutput = page.locator('#terminal-output');

    // Assicurati che il toggle (label) sia visibile e l'input checkato di default
    const switchLabel = page.locator('.auto-refresh .switch');
    await expect(switchLabel).toBeVisible();
    await expect(toggleAutoRefresh).toBeChecked();

    // Clicca il toggle per spegnerlo (pause)
    await switchLabel.click();
    
    // Verifica che appaia il messaggio "Auto-refresh disabled"
    await expect(terminalOutput).toContainText('Auto-refresh disabled');

    // Cliccalo di nuovo per riaccenderlo (resume)
    await switchLabel.click();

    // Verifica che appaia "Auto-refresh enabled"
    await expect(terminalOutput).toContainText('Auto-refresh enabled');
  });

  test('should trigger USB diagnostics download', async ({ page }) => {
    await page.route('**/api/usb/dump', async route => {
      await route.fulfill({
        status: 200,
        headers: {
          'Content-Type': 'application/json',
          'Content-Disposition': 'attachment; filename="usb_diagnostics.json"'
        },
        body: JSON.stringify({ driver: "Generic", vid: "0x1234", pid: "0x5678" })
      });
    });

    await page.goto('/');
    await page.click('[data-target="logs"]');

    const btnExportUsb = page.locator('#link-export-usb');
    await expect(btnExportUsb).toBeVisible();

    const requestPromise = page.waitForRequest(request => request.url().includes('/api/usb/dump'));
    await btnExportUsb.click();
    await requestPromise;

    const terminalOutput = page.locator('#terminal-output');
    await expect(terminalOutput).toContainText('Downloading USB diagnostics...');
  });
});
