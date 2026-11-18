// Bundles superdough's worklets.mjs (unmodified) into one file for node-web-audio-api's
// audioWorklet.addModule(), as vite's ?audioworklet does for the browser. kabelsalat's ugens
// (K() / generic-processor) come from js/node_modules.
import * as esbuild from 'esbuild';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const sd = path.resolve(here, '../../third_party/strudel/packages/superdough');
await esbuild.build({
  entryPoints: [path.join(sd, 'worklets.mjs')],
  outfile: process.argv[2],
  bundle: true,
  format: 'esm',
  platform: 'neutral',
  logLevel: 'error',
  define: { 'process.env.NODE_ENV': '"production"' },
  nodePaths: [path.resolve(here, '../node_modules')], // @kabelsalat/lib's ugens (K())
});
console.log(`wrote ${process.argv[2]}`);
