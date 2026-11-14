// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Preserve the independently sampled UA paint/compositing and live cursor evidence.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const scales = [100, 150, 200].map(label => {
  const directory = path.join(root, `../build/ant-design-gpui/pagination-reference/go-paint-${label}`);
  const filename = path.join(directory, 'go-paint-probe.json');
  const data = JSON.parse(fs.readFileSync(filename, 'utf8'));
  if (data.scale !== label / 100 || data.reference_version !== '6.6.7' || data.rc_pagination_version !== '1.4.0' ||
      data.records.length !== 10 || data.cursor_records.length !== 4 ||
      !data.edge_band_model?.passed || data.edge_band_model.maximum_error_rgb_bytes > 1 ||
      data.records.some(record => Math.max(...record.fill_prediction_error.map(Math.abs), ...record.border_prediction_error.map(Math.abs)) > 1) ||
      data.cursor_records.some(record => record.cursor.button !== 'default')) throw Error(`Incomplete UA evidence at ${label}%`);
  const { source_provenance, records, ...metadata } = data;
  void source_provenance;
  return { ...metadata, evidence_sha256: digest(filename), records: records.map(record => {
    const { png, ...evidence } = record;
    if (digest(png) !== record.png_sha256) throw Error(`PNG changed: ${png}`);
    return { ...evidence, png_relative_to_probe: path.relative(directory, png).replaceAll('\\', '/') };
  }) };
});
const fixture = { description: 'Actual original browser PNG samples over five backgrounds under two themes at three scales; effective two-point sample fits reproduce all samples within one RGB byte. Quantized two-point fits do not uniquely identify intrinsic RGBA. Resolved border bands are validated against recorded CSS RGBA and actual pixels. Cursor records are live computed DOM cursor styles.', scales };
fs.writeFileSync(path.join(root, 'fixtures/pagination/native-form-reference.json'), JSON.stringify(fixture, null, 2) + '\n');
console.log('Packed 30 actual native-form backdrop samples and 12 original cursor records');
