import { defineConfig } from '@playwright/test';
import config from './playwright.config';

// Regenerates docs/images/ui-*.png: npx playwright test -c playwright.screenshots.config.ts
export default defineConfig(config, { testMatch: 'screenshot.spec.ts', testIgnore: [] });
