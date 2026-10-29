// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Loopback-only, process-owned fixture server. Never accesses signed-in browser state.
import http from 'node:http';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export async function createImageFixtureServer(options = {}) {
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
  const reference = path.resolve(options.reference ?? path.join(root, '../build/ant-design-gpui/image-reference/browser'));
  const assets = path.resolve(options.assets ?? path.join(root, 'resources/image'));
  const held = new Map(), counts = new Map();
  async function image(name, response) {
    try {
      const bytes = await fs.readFile(path.join(assets, `${name}.png`));
      response.writeHead(200, { 'content-type': 'image/png', 'cache-control': 'no-store' }); response.end(bytes);
    } catch { response.writeHead(404); response.end('Missing fixture image'); }
  }
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1');
    counts.set(url.pathname, (counts.get(url.pathname) ?? 0) + 1);
    const match = /^\/(images|held|delay\/([0-9]+))\/([a-z-]+)\.png$/.exec(url.pathname);
    if (match) {
      if (match[1] === 'held') {
        const requests = held.get(match[3]) ?? []; requests.push(response); held.set(match[3], requests);
      } else if (match[2]) setTimeout(() => image(match[3], response), Math.min(Number(match[2]), 10000));
      else await image(match[3], response);
      return;
    }
    const file = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
    if (!['index.html', 'reference.js', 'reference.css', 'reference.js.map', 'reference.css.map'].includes(file)) {
      response.writeHead(404); response.end('Unknown fixture endpoint'); return;
    }
    try {
      const bytes = await fs.readFile(path.join(reference, file));
      response.writeHead(200, { 'content-type': file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html', 'cache-control': 'no-store' }); response.end(bytes);
    } catch { response.writeHead(404); response.end('Build the Image reference first'); }
  });
  await new Promise(resolve => server.listen(options.port ?? 0, '127.0.0.1', resolve));
  return { url: `http://127.0.0.1:${server.address().port}`, counts,
    async release(name) { for (const response of held.get(name) ?? []) await image(name, response); held.delete(name); },
    async close() { for (const responses of held.values()) for (const response of responses) response.destroy(); server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); },
  };
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const fixture = await createImageFixtureServer(); console.log(fixture.url);
  process.on('SIGINT', async () => { await fixture.close(); process.exit(0); });
}
