#!/usr/bin/env node

const fs = require('fs');
const http = require('http');
const path = require('path');
const { spawn, spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');

function arg(name, def) {
  const idx = process.argv.indexOf(`--${name}`);
  if (idx >= 0 && idx + 1 < process.argv.length) return process.argv[idx + 1];
  return def;
}

const workers = arg('workers', '1,2,4,8,16,32').split(',').map((v) => parseInt(v, 10));
const count = parseInt(arg('count', '1000000'), 10);
const conc = parseInt(arg('conc', '128'), 10);
const basePort = parseInt(arg('port', '19080'), 10);
const modes = arg('modes', 'write,read,mixed').split(',');

function runMake() {
  const res = spawnSync('make', ['server'], { cwd: root, stdio: 'inherit' });
  if (res.status !== 0) process.exit(res.status || 1);
}

function request(port, method, route, body) {
  return new Promise((resolve, reject) => {
    const text = body ? JSON.stringify(body) : '';
    const req = http.request({
      hostname: '127.0.0.1',
      port,
      path: route,
      method,
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(text),
      },
    }, (res) => {
      let data = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        try {
          resolve({ code: res.statusCode, json: JSON.parse(data) });
        } catch (err) {
          reject(new Error(`bad json from ${route}: ${data}`));
        }
      });
    });
    req.on('error', reject);
    if (text) req.write(text);
    req.end();
  });
}

async function waitHealth(port) {
  const end = Date.now() + 7000;
  while (Date.now() < end) {
    try {
      const res = await request(port, 'GET', '/health');
      if (res.json.ok) return;
    } catch (err) {
      await new Promise((r) => setTimeout(r, 100));
    }
  }
  throw new Error(`server on port ${port} did not become healthy`);
}

async function sql(port, text) {
  const res = await request(port, 'POST', '/sql', { sql: text });
  if (!res.json.ok) throw new Error(`sql failed: ${text} -> ${JSON.stringify(res.json)}`);
  return res.json;
}

async function seed(port) {
  await sql(port, 'CREATE TABLE books;');
  const todo = [];
  for (let i = 1; i <= 1000; i += 1) {
    todo.push(`INSERT INTO books VALUES (${i}, 'seed title ${i}', 'seed author', 2024);`);
  }
  for (let i = 0; i < todo.length; i += 1) {
    await sql(port, todo[i]);
  }
}

function makeSql(mode, i, worker) {
  if (mode === 'write') {
    const id = worker * 100000000 + i;
    return `INSERT INTO books VALUES (${id}, 'title ${i}', 'author ${worker}', 2024);`;
  }
  if (mode === 'read') {
    const id = (i % 1000) + 1;
    return `SELECT * FROM books WHERE id = ${id};`;
  }
  if (i % 2 === 0) {
    const id = (i % 1000) + 1;
    return `SELECT * FROM books WHERE id = ${id};`;
  }
  return `INSERT INTO books VALUES (${worker * 200000000 + i}, 'mix title ${i}', 'mix author', 2024);`;
}

async function runMode(port, worker, mode) {
  if (mode === 'write') {
    await sql(port, 'CREATE TABLE books;');
  } else {
    await seed(port);
  }

  let next = 1;
  let done = 0;
  let waitSum = 0;
  let workSum = 0;
  const started = Date.now();

  async function one() {
    while (next <= count) {
      const i = next;
      next += 1;
      const res = await request(port, 'POST', '/sql', { sql: makeSql(mode, i, worker) });
      if (!res.json.ok) {
        throw new Error(`request failed: ${JSON.stringify(res.json)}`);
      }
      waitSum += res.json.wait_ms || 0;
      workSum += res.json.work_ms || 0;
      done += 1;
    }
  }

  const tasks = [];
  const n = Math.min(conc, count);
  for (let i = 0; i < n; i += 1) tasks.push(one());
  await Promise.all(tasks);

  const total = Date.now() - started;
  return {
    workers: worker,
    mode,
    count: done,
    conc,
    total_ms: total,
    avg_wait_ms: done ? waitSum / done : 0,
    avg_work_ms: done ? workSum / done : 0,
    qps: total > 0 ? done * 1000 / total : 0,
  };
}

async function runWorker(worker, index) {
  const port = basePort + index;
  const child = spawn(path.join(root, 'server'), ['--port', String(port), '--workers', String(worker)], {
    cwd: root,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.on('data', (d) => process.stdout.write(`[server ${worker}] ${d}`));
  child.stderr.on('data', (d) => process.stderr.write(`[server ${worker}] ${d}`));

  try {
    await waitHealth(port);
    const rows = [];
    for (const mode of modes) {
      console.log(`bench workers=${worker} mode=${mode} count=${count} conc=${conc}`);
      rows.push(await runMode(port, worker, mode));
    }
    return rows;
  } finally {
    child.kill('SIGTERM');
    await new Promise((resolve) => child.once('exit', resolve));
  }
}

async function main() {
  runMake();
  const all = [];
  for (let i = 0; i < workers.length; i += 1) {
    const rows = await runWorker(workers[i], i);
    all.push(...rows);
  }
  const out = path.join(root, 'bench', 'result.json');
  fs.writeFileSync(out, `${JSON.stringify(all, null, 2)}\n`);
  console.log(`wrote ${out}`);
}

main().catch((err) => {
  console.error(err.stack || err.message);
  process.exit(1);
});
