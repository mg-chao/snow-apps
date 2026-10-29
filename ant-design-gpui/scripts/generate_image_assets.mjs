// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Original deterministic RGBA artwork shared by both independent renderers.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.join(root, 'resources/image');
fs.mkdirSync(output, { recursive: true });
const crcTable = Array.from({ length: 256 }, (_, value) => {
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
  return value >>> 0;
});
function chunk(type, data) {
  const tag = Buffer.from(type), body = Buffer.concat([tag, data]);
  let crc = 0xffffffff;
  for (const value of body) crc = crcTable[(crc ^ value) & 255] ^ (crc >>> 8);
  const header = Buffer.alloc(4), trailer = Buffer.alloc(4);
  header.writeUInt32BE(data.length); trailer.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([header, body, trailer]);
}
const mix = (a, b, t) => a.map((value, index) => Math.round(value + (b[index] - value) * t));
const circle = (x, y, cx, cy, radius) => (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2;
function rgba(name, x, y) {
  if (name === 'landscape') {
    if (circle(x, y, .76, .25, .12)) return [255, 205, 115, 255];
    if (y > .78 + .13 * Math.sin(x * 7 + 1)) return [30, 91, 111, 255];
    if (y > .67 + .2 * Math.sin(x * 5 - .5)) return [62, 129, 141, 255];
    if (y > .57 + .13 * Math.sin(x * 8)) return [116, 169, 170, 255];
    return [...mix([173, 214, 232], [231, 237, 220], y), 255];
  }
  if (name === 'portrait') {
    if (circle(x, y, .28, .19, .15)) return [244, 188, 115, 255];
    if (x > .4 && x < .78 && y > .43 && y < .9) return [47, 95, 107, 255];
    if (circle(x, y, .59, .43, .19)) return [47, 95, 107, 255];
    if (y > .8 + x * .1) return [93, 142, 136, 255];
    if (x < .18 && y > .3 && y < .72) return [208, 118, 103, 255];
    return [...mix([237, 216, 184], [225, 192, 149], y), 255];
  }
  if (name === 'transparent') {
    if (x > .1 && x < .28 && y > .16 && y < .82) return [22, 119, 255, 255];
    if (x > .28 && x < .67 && y > .65 && y < .82) return [22, 119, 255, 255];
    if (circle(x, y, .7, .32, .17)) return [250, 140, 22, 210];
    if (x > .35 && x < .59 && y > .26 && y < .5) return [82, 196, 26, 150];
    return [0, 0, 0, 0];
  }
  if (circle(x, y, .32, .32, .055)) return [140, 140, 140, 255];
  if (x > .18 && x < .82 && y > .22 && y < .78) {
    if (x < .195 || x > .805 || y < .235 || y > .765) return [140, 140, 140, 255];
    if (y > .66 - .32 * Math.sin((x - .2) * 7)) return [189, 189, 189, 255];
  }
  return [245, 245, 245, 255];
}
const manifest = [];
for (const [name, width, height] of [['landscape', 320, 200], ['portrait', 300, 560], ['transparent', 400, 260], ['fallback', 200, 200]]) {
  const pixels = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const color = [0, 0, 0, 0];
    for (let sy = 0; sy < 3; sy++) for (let sx = 0; sx < 3; sx++) {
      const sample = rgba(name, (x + (sx + .5) / 3) / width, (y + (sy + .5) / 3) / height);
      for (let channel = 0; channel < 4; channel++) color[channel] += sample[channel] / 9;
    }
    for (let channel = 0; channel < 4; channel++) pixels[y * (width * 4 + 1) + 1 + x * 4 + channel] = Math.round(color[channel]);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  const bytes = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(pixels, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
  fs.writeFileSync(path.join(output, `${name}.png`), bytes);
  manifest.push({ name, width, height, transparent: name === 'transparent', sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
}
fs.writeFileSync(path.join(output, 'manifest.json'), JSON.stringify({ license: 'Apache-2.0', generator: 'scripts/generate_image_assets.mjs', images: manifest }, null, 2) + '\n');
console.log(`Generated ${manifest.length} original Image fixtures`);
