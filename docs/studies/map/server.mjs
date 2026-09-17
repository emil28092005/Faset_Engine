import http from 'node:http';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readDocument } from './file-access.mjs';
const here = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(here, '../../..');
const port = Number(process.env.PORT || 4178);
const server = http.createServer(async (req,res) => {
  res.setHeader('X-Content-Type-Options','nosniff');
  res.setHeader('Cache-Control','no-store');
  res.setHeader('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self'; connect-src 'self'; frame-ancestors 'self'");
  if (!['localhost:'+port,'127.0.0.1:'+port].includes(req.headers.host)) {res.writeHead(403);res.end();return;}
  if (req.method !== 'GET' && req.method !== 'HEAD') {res.writeHead(405);res.end();return;}
  try {
    const url = new URL(req.url,'http://localhost:'+port);
    let data, mime;
    if (url.pathname === '/api/file') {
      data = JSON.stringify(await readDocument(projectRoot, url.searchParams.get('path')));mime='application/json; charset=utf-8';
    } else {
      const files = {'/':['index.html','text/html; charset=utf-8'],'/app.js':['app.js','text/javascript; charset=utf-8'],'/app.css':['app.css','text/css; charset=utf-8']};
      const entry = files[url.pathname];
      if (!entry) {res.writeHead(404);res.end();return;}
      data=await readFile(path.join(here,'dist',entry[0]));mime=entry[1];
    }
    res.writeHead(200,{'Content-Type':mime});res.end(req.method === 'HEAD' ? undefined : data);
  } catch (error) {res.writeHead(error.status === 403 ? 403 : 404);res.end(error.status === 403 ? 'File not allowed' : 'File not found');}
});
server.listen(port,'127.0.0.1',()=>console.log('Research map: http://localhost:'+port));
