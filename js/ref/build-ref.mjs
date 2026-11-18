// Bundles js/ref/superdough-render.mjs for node (see that file).
import * as esbuild from 'esbuild';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const packages = path.resolve(here, '../../third_party/strudel/packages');
const out = process.argv[2];

const plugin = {
  name: 'strudel-ref',
  setup(build) {
    // reverbGen.mjs: Math.random() -> the same xorshift32 as Concha's generateReverb, seeded like it
    // (orbit 1, one seed per generated response), so both impulse responses are identical.
    build.onLoad({ filter: /reverbGen\.mjs$/ }, (args) => {
      let src = fs.readFileSync(args.path, 'utf8');
      src = src.replace(/Math\.random\(\)/g, 'globalThis.__conchaReverbRandom()');
      src = src.replace('reverbGen.generateReverb = function (params, callback) {',
        'reverbGen.generateReverb = function (params, callback) {\n  globalThis.__conchaReverbStart();');
      return { contents: src, loader: 'js' };
    });
    build.onResolve({ filter: /\?audioworklet$/ }, () => ({ path: 'worklets', namespace: 'stub' }));
    // @kabelsalat/web (its SalatRepl wants a browser): superdough's compileKabel gets the same
    // evaluation through globalThis.__conchaKabel (set by superdough-render.mjs)
    build.onResolve({ filter: /^@kabelsalat\/web/ }, () => ({ path: 'kabelsalat', namespace: 'stub' }));
    build.onLoad({ filter: /.*/, namespace: 'stub' }, (a) => ({
      contents: a.path === 'worklets' ? 'export default "worklets-not-loaded";' : 'export const SalatRepl = class {}; export default {};',
    }));
    build.onLoad({ filter: /superdough[\\/]superdough\.mjs$/ }, (args) => {
      let src = fs.readFileSync(args.path, 'utf8');
      src = src.replace("const compileKabel = (code) => {", "const compileKabel = (code) => {\n  kabel ??= globalThis.__conchaKabel;");
      return { contents: src, loader: 'js' };
    });
    build.onResolve({ filter: /^@strudel\// }, (args) => {
      const [, name, ...rest] = args.path.split('/');
      const dir = path.join(packages, name);
      if (rest.length) return { path: path.join(dir, ...rest) };
      const pkg = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
      return { path: path.join(dir, pkg.main || 'index.mjs') };
    });
  },
};

await esbuild.build({
  entryPoints: [path.join(here, 'superdough-render.mjs')],
  outfile: out,
  bundle: true,
  format: 'esm',
  platform: 'node',
  mainFields: ['module', 'main'], // @kabelsalat/core: its "main" is an IIFE build
  target: 'node20',
  external: ['node-web-audio-api'],
  nodePaths: [path.join(here, '../node_modules')],
  plugins: [plugin],
  logLevel: 'error',
  define: { 'process.env.NODE_ENV': '"production"' },
  banner: { js: "import { createRequire } from 'module'; const require = createRequire(import.meta.url);" },
});
console.log(`wrote ${out}`);
