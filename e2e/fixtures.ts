import { test as base } from '@playwright/test';
import * as fs from 'fs';
import * as path from 'path';

const www = path.resolve(__dirname, '../data/www');

// The firmware's page routes; other paths are looked up as files in data/www
const pages: Record<string, string> = { '/': 'index.html', '/update': 'update.html' };

// Stands in for the board at baseURL: GETs are served from data/www and the rest
// get the firmware's 404. Specs mock /api/* with page.route, which runs first.
export const test = base.extend({
  context: async ({ context, baseURL }, use) => {
    await context.route(`${baseURL}/**`, route => {
      const request = route.request();
      const { pathname } = new URL(request.url());
      const file = path.join(www, pages[pathname] ?? pathname);
      if (request.method() === 'GET' && fs.statSync(file, { throwIfNoEntry: false })?.isFile()) {
        return route.fulfill({ path: file });
      }
      return route.fulfill({ status: 404, contentType: 'text/plain', body: 'Not found' });
    });
    await use(context);
  },
});

export { expect } from '@playwright/test';
