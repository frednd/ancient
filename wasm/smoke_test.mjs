// Smoke test for the WebAssembly build: node wasm/smoke_test.mjs [dist-dir]
// Scans single test files and a concatenated blob, and checks decompression
// against testing/test_files/test_C1.raw.
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = process.argv[2] || join(root, 'wasm', 'dist');
const { default: createAncient } = await import(pathToFileURL(join(dist, 'ancient.mjs')).href);
const M = await createAncient();
const files = join(root, 'testing', 'test_files');

function withBuffer(bytes, fn) {
  const p = M._anc_malloc(bytes.length);
  M.HEAPU8.set(bytes, p);
  try { return fn(p); } finally { M._anc_free(p); }
}
const scan = (b) => withBuffer(b, (p) => JSON.parse(M.UTF8ToString(M._anc_scan(p, b.length, 0, b.length, 1))));
const decompress = (b) => withBuffer(b, (p) => {
  const n = M._anc_decompress(p, b.length, 1);
  return n < 0 ? null : M.HEAPU8.slice(M._anc_out_ptr(), M._anc_out_ptr() + n);
});

const raw = readFileSync(join(files, 'test_C1.raw'));
const samples = ['test_C1.pp', 'test_C1.imp', 'test_C1.crm', 'test_C1.rnc1', 'test_C1.grnd', 'test_C1.tpwm', 'test_C1.bz2', 'test_C1.byte_1.2', 'test_C1.byte_2.0', 'test_C1.byte_3.0', 'test_C1.bytepro_1.0'];
let failed = 0;
const check = (ok, msg) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${msg}`); if (!ok) failed++; };

const parts = [];
const names = [];
for (const name of samples) {
  let data;
  try { data = readFileSync(join(files, name)); } catch { console.log(`skip ${name} (missing)`); continue; }
  const out = decompress(data);
  check(out && Buffer.compare(Buffer.from(out), raw) === 0, `${name}: decompresses to test_C1.raw`);
  const r = scan(data);
  if (name !== 'test_C1.pp') check(r.hits.length >= 1 && r.hits[0].offset === 0, `${name}: scan finds stream at 0 (${r.hits[0]?.name ?? 'none'})`);
  parts.push(data);
  names.push(name);
}

// Concatenate streams with filler in between and check offsets.
const filler = Buffer.alloc(1000, 0x4e);
const expected = [];
const chunks = [];
let off = 0;
for (const p of parts) {
  chunks.push(filler); off += filler.length;
  expected.push(off); chunks.push(p); off += p.length;
}
const blob = Buffer.concat(chunks);
const found = scan(blob).hits.map((h) => h.offset);
// PowerPacker keeps its header at the end and GRND has no end marker, so the
// scanner cannot find them inside a blob; the web app handles those separately.
const noScan = new Set(['test_C1.pp', 'test_C1.grnd']);
parts.forEach((p, k) => {
  if (!noScan.has(names[k])) check(found.includes(expected[k]), `blob: ${names[k]} found at ${expected[k]}`);
});

// End search for PowerPacker inside the blob: the true length must verify.
const ppAt = expected[names.indexOf('test_C1.pp')];
const ppLen = readFileSync(join(files, 'test_C1.pp')).length;
const tryExact = (b, off, len) => withBuffer(b.subarray(off, off + len), (p) => M._anc_try_exact(p, len));
check(tryExact(blob, ppAt, ppLen) === raw.length, 'blob: PowerPacker verifies with its true length');
check(tryExact(blob, ppAt, ppLen + 4) < 0 && tryExact(blob, ppAt, ppLen - 4) < 0, 'blob: PowerPacker rejects wrong lengths');
console.log(failed ? `${failed} failures` : 'all passed');
process.exit(failed ? 1 : 0);
