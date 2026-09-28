import { test, expect } from './fixtures';

test.describe('Responsive Web UI', () => {
  // Impostiamo la viewport a una risoluzione tipica per mobile
  test.use({ viewport: { width: 375, height: 812 } });

  test('should display mobile layout and prevent horizontal scroll', async ({ page }) => {
    await page.goto('/');

    const appContainer = page.locator('.app-container');
    await expect(appContainer).toBeVisible();

    // Verifichiamo che i container principali abbiano ricevuto il layout a colonna
    const containerStyle = await appContainer.evaluate((el) => window.getComputedStyle(el).flexDirection);
    expect(containerStyle).toBe('column');

    // Verifichiamo che non ci sia scroll orizzontale (nessun elemento eccede la larghezza)
    const scrollWidth = await page.evaluate(() => document.documentElement.scrollWidth);
    const clientWidth = await page.evaluate(() => document.documentElement.clientWidth);
    
    expect(scrollWidth).toBeLessThanOrEqual(clientWidth);
  });
});
