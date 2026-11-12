// jit.js: a JIT-heavy Node workload. Each kernel runs ROUNDS rounds and prints each round's time, so the
// first rounds (translation, tier-up, code patching) separate from steady state; the checksum must match native.
'use strict';
const ROUNDS = +(process.argv[2] || 8), SCALE = +(process.argv[3] || 1);
let check = 0;
const mix = (x) => { check = (Math.imul(check ^ (x | 0), 0x9E3779B1) + 0x7F4A7C15) | 0; };

// Polymorphic objects and virtual calls.
class Shape { area() { return 0; } }
class Sq extends Shape { constructor(s) { super(); this.s = s; } area() { return this.s * this.s; } }
class Ci extends Shape { constructor(r) { super(); this.r = r; } area() { return 3 * this.r * this.r; } }
class Tri extends Shape { constructor(b, h) { super(); this.b = b; this.h = h; } area() { return (this.b * this.h) >> 1; } }
function objects() {
  let s = 0;
  for (let i = 0; i < 300000 * SCALE; i++) {
    const o = i % 3 === 0 ? new Sq(i & 63) : i % 3 === 1 ? new Ci(i & 31) : new Tri(i & 127, i & 15);
    s = (s + o.area()) | 0;
  }
  return s;
}

// Integer bit operations: a SHA-256 round function over a buffer.
const K = new Int32Array(64).map((_, i) => (Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32)) | 0);
function sha(n) {
  const w = new Int32Array(64);
  let a = 0x6a09e667, b = 0xbb67ae85 | 0, c = 0x3c6ef372, d = 0xa54ff53a | 0, e = 0x510e527f, f = 0x9b05688c | 0, g = 0x1f83d9ab, h = 0x5be0cd19;
  for (let blk = 0; blk < n; blk++) {
    for (let i = 0; i < 16; i++) w[i] = Math.imul(blk + i, 0x01000193) ^ (i << 7);
    for (let i = 16; i < 64; i++) {
      const x = w[i - 15], y = w[i - 2];
      const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
      const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const t1 = (h + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
  }
  return a ^ e ^ Math.clz32(h | 1);
}

// Floating point over typed arrays: an n-body step.
function nbody(steps) {
  const n = 64, x = new Float64Array(n), y = new Float64Array(n), vx = new Float64Array(n), vy = new Float64Array(n);
  for (let i = 0; i < n; i++) { x[i] = Math.cos(i); y[i] = Math.sin(i * 1.3); }
  for (let s = 0; s < steps; s++) {
    for (let i = 0; i < n; i++) {
      let ax = 0, ay = 0;
      for (let j = 0; j < n; j++) {
        const dx = x[j] - x[i], dy = y[j] - y[i], r2 = dx * dx + dy * dy + 0.01, inv = 1 / (r2 * Math.sqrt(r2));
        ax += dx * inv; ay += dy * inv;
      }
      vx[i] += ax * 1e-5; vy[i] += ay * 1e-5;
    }
    for (let i = 0; i < n; i++) { x[i] += vx[i]; y[i] += vy[i]; }
  }
  return Math.round(x[0] * 1e9) ^ Math.round(y[7] * 1e9);
}

// Strings, regular expressions, JSON.
function strings() {
  const parts = [];
  for (let i = 0; i < 20000 * SCALE; i++) parts.push({ id: i, name: 'item-' + i.toString(36), tags: ['a' + (i % 7), 'b' + (i % 11)] });
  const js = JSON.stringify(parts), back = JSON.parse(js);
  let m = 0;
  const re = /item-([0-9a-z]+)/g;
  for (const r of js.matchAll(re)) m += r[1].length;
  return back.length ^ m ^ js.length;
}

// Maps, closures, sort.
function collections() {
  const m = new Map();
  for (let i = 0; i < 100000 * SCALE; i++) { const k = 'k' + (Math.imul(i, 2654435761) >>> 20); m.set(k, (m.get(k) || 0) + 1); }
  const arr = [...m.entries()].sort((p, q) => q[1] - p[1] || (p[0] < q[0] ? -1 : 1));
  return arr.length ^ arr[0][1] ^ arr.slice(0, 50).reduce((s, e) => s + e[0].length, 0);
}

// Code churn: many distinct functions, each called enough to be optimized, then dropped.
function churn(round) {
  let s = 0;
  for (let k = 0; k < 150 * SCALE; k++) {
    const f = new Function('x', `let s = ${k + round}; for (let i = 0; i < x; i++) s = (s * 31 + (i ^ ${k})) | 0; return s;`);
    for (let r = 0; r < 40; r++) s = (s + f(200)) | 0;
  }
  return s;
}

const kernels = [['objects', objects], ['sha', () => sha(20000 * SCALE)], ['nbody', () => nbody(300 * SCALE)],
                 ['strings', strings], ['collections', collections], ['churn', churn]];
const t00 = process.hrtime.bigint();
for (const [name, fn] of kernels) {
  const ts = [];
  for (let r = 0; r < ROUNDS; r++) {
    const t0 = process.hrtime.bigint();
    mix(fn(r));
    ts.push(Number(process.hrtime.bigint() - t0) / 1e6);
  }
  console.log(`round ${name} ${ts.map((t) => t.toFixed(1)).join(' ')} ms`);
}
console.log(`total ${(Number(process.hrtime.bigint() - t00) / 1e6).toFixed(0)} ms`);
console.log(`checksum ${(check >>> 0).toString(16)}`);
