import { build } from 'esbuild';
import { copyFile, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
const here = fileURLToPath(new URL('.', import.meta.url));
await mkdir(new URL('./dist/', import.meta.url), {recursive:true});
await build({absWorkingDir:here,entryPoints:['main.tsx'],bundle:true,outfile:'dist/app.js',jsx:'automatic',minify:true,alias:{'cursor/canvas':'./canvas-browser.tsx'},define:{'process.env.NODE_ENV':'"production"'}});
await copyFile(new URL('./index.html', import.meta.url), new URL('./dist/index.html', import.meta.url));
