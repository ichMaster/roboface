/* Render every panel headlessly and report where the ink actually landed.
 *
 * Written for one failure: an SVG does not clip by default, so a series computed
 * against a hardcoded axis maximum leaves its own card and paints over the panel
 * above it. On the prototype's four-version mock every constant fit, so the charts
 * looked correct right up until a real ten-version run went through them.
 *
 * Reads app.js unmodified, stubs the few DOM calls it makes, drives it with a state
 * object on stdin, and prints one JSON summary per panel: the viewBox, the extreme
 * coordinates, and the bar heights. The assertions live in Python.
 */
const fs = require('fs');
const path = require('path');

const source = fs.readFileSync(
  path.join(__dirname, '..', 'dashboard', 'static', 'app.js'), 'utf8');
const state = JSON.parse(fs.readFileSync(0, 'utf8'));

const sinks = {};
const node = (id) => (sinks[id] ??= {
  id, innerHTML: '', textContent: '', dataset: {},
  className: '', hidden: false,
  querySelectorAll: () => [], addEventListener: () => {},
  setAttribute: () => {}, getAttribute: () => null,
  appendChild: () => {}, contains: () => false,
});

globalThis.document = {
  getElementById: node,
  querySelector: node,
  querySelectorAll: () => [],
  documentElement: { setAttribute: () => {}, getAttribute: () => null },
  createElement: () => node('scratch'),
  createTextNode: () => ({}),
  activeElement: null,
  body: {},
};
globalThis.window = globalThis;
globalThis.location = { protocol: 'http:', host: 'x' };
globalThis.WebSocket = function () { return { close() {} }; };
globalThis.matchMedia = () => ({ matches: false });
globalThis.setTimeout = (fn) => { fn(); return 0; };
globalThis.getComputedStyle = () => ({ getPropertyValue: () => '#000000' });

// app.js is one script scope, so appending a driver is enough to reach its internals
// without exporting anything or altering the file under test.
// The driver runs INSIDE app.js's scope, which is the only place its helpers exist --
// mmss() is used by three panels and the header, so a malformed clock is a whole-page
// fault rather than one chart's, and it is worth asking about here.
const clock = new Function('__state', source + `
  ;STATE = __state; renderAll();
  const bad = [];
  for (let s = 0; s < 7200; s += 0.1) {
    const t = mmss(s);
    if (!/^\\d+:[0-5]\\d$/.test(t)) bad.push([Math.round(s * 10) / 10, t]);
  }
  return bad;`)(state);

// The lookbehind matters: a bare /x="/ also matches rx=" and a bare /y="/ matches cy=",
// which would fold corner radii into the coordinate extremes and hide a real overflow.
const nums = (svg, attr) => [...svg.matchAll(new RegExp(`(?<![a-zA-Z])${attr}="(-?[\\d.]+)"`, 'g'))]
  .map(m => parseFloat(m[1])).filter(Number.isFinite);

const report = {};
for (const id of ['c-burn', 'c-vel', 'c-time', 'c-fail', 'c-suite', 'c-q']) {
  const html = sinks[id]?.innerHTML || '';
  const box = html.match(/viewBox="0 0 ([\d.]+) ([\d.]+)"/);
  if (!box) { report[id] = { rendered: false, html: html.slice(0, 120) }; continue; }
  // Every y-ish coordinate the panels emit, plus path data, which carries the rest.
  const ys = [...nums(html, 'y'), ...nums(html, 'y1'), ...nums(html, 'y2'),
              ...nums(html, 'cy')];
  const xs = [...nums(html, 'x'), ...nums(html, 'x1'), ...nums(html, 'x2'),
              ...nums(html, 'cx')];
  const pathYs = [...html.matchAll(/[ML,](-?[\d.]+),(-?[\d.]+)/g)].map(m => parseFloat(m[2]));
  report[id] = {
    rendered: true,
    width: parseFloat(box[1]), height: parseFloat(box[2]),
    min_y: Math.min(...ys, ...pathYs), max_y: Math.max(...ys, ...pathYs),
    min_x: Math.min(...xs), max_x: Math.max(...xs),
    bar_heights: nums(html, 'height'),
    // Plotted point heights. A series fed one repeated value draws a dead-flat line
    // that reads as a measurement -- distinguishable from real data only here.
    point_ys: nums(html, 'cy'),
    label_rows: [...html.matchAll(/class="axl"[^>]*y="([\d.]+)"/g)].map(m => parseFloat(m[1])),
    // Category labels on the x-axis -- the ones that can collide with a neighbour.
    // Marked `xcat` at the source so this cannot accidentally sweep in y-axis ticks,
    // which share the class and the anchor but never share an x.
    x_labels: [...html.matchAll(/<text class="ax xcat"([^>]*)>([^<]*)<\/text>/g)].map(m => ({
      x: parseFloat((m[1].match(/(?<![a-zA-Z])x="(-?[\d.]+)"/) || [])[1]),
      text: m[2].trim(),
      rotated: /rotate\(/.test(m[1]),
    })),
  };
}
report._clock = clock;
process.stdout.write(JSON.stringify(report));
