// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Private, isolated headless browser transport shared by Pagination evidence tools.
import fs from 'node:fs/promises';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
export async function browser({ output, scale = 1.5, edge = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe' }) {
  await fs.mkdir(output, { recursive: true });
  const profile = path.join(output, `profile-${randomUUID()}`);
  const child = spawn(edge, ['--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
    '--disable-extensions', '--disable-background-networking', '--disable-component-update',
    '--remote-debugging-port=0', `--force-device-scale-factor=${scale}`, `--user-data-dir=${profile}`, 'about:blank'],
  { windowsHide: true, stdio: ['ignore', 'ignore', 'pipe'] });
  let stderr = '', launchError, socket, sequence = 0;
  const pending = new Map();
  child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-32768); });
  child.on('error', error => { launchError = error; });
  async function command(method, params = {}, sessionId) {
    const id = ++sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { pending.delete(id); reject(Error(`Timeout: ${method}`)); }, 30000);
      pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    });
  }
  async function close() {
    if (socket?.readyState === WebSocket.OPEN) await command('Browser.close').catch(() => {});
    socket?.close();
    if (child.exitCode === null) child.kill();
    await fs.writeFile(path.join(output, 'browser-stderr.txt'), stderr);
  }
  try {
    let port;
    for (let i = 0; i < 300; i++) {
      if (launchError) throw launchError;
      if (child.exitCode !== null) throw Error(`Headless browser exited: ${stderr}`);
      try { port = (await fs.readFile(path.join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; break; } catch {}
      await delay(100);
    }
    if (!port) throw Error(`Browser startup timed out: ${stderr}`);
    const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
    socket = new WebSocket(version.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
    socket.onmessage = event => {
      const response = JSON.parse(event.data), request = pending.get(response.id);
      if (!request) return;
      pending.delete(response.id); clearTimeout(request.timer);
      if (response.error) request.reject(Error(JSON.stringify(response.error))); else request.resolve(response.result);
    };
    const { targetId } = await command('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await command('Target.attachToTarget', { targetId, flatten: true });
    const page = (method, params) => command(method, params, sessionId);
    await page('Page.enable'); await page('Runtime.enable');
    async function evaluate(expression, awaitPromise = false) {
      const result = await page('Runtime.evaluate', { expression, returnByValue: true, awaitPromise });
      if (result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
      return result.result.value;
    }
    async function settle() { await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))', true); }
    return { page, evaluate, settle, close, renderer: version.Browser };
  } catch (error) { await close(); throw error; }
}
