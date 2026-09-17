import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, symlink, rm, truncate } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { readDocument } from './file-access.mjs';
import { resolveDocumentLink } from './file-links.mjs';

test('relative report/source links preserve document context and source lines', () => {
  const from = 'docs/studies/14-engine-blueprint.md';
  const cases = [
    ['09-unity-ux-extensibility-study.md', 'docs/studies/09-unity-ux-extensibility-study.md', null],
    ['../ARCHITECTURE.md', 'docs/ARCHITECTURE.md', null],
    ['../../../UnrealEngine/Engine/example.cpp:123', '../UnrealEngine/Engine/example.cpp', '123'],
    ['../../../godot/scene/main/node.cpp#L77', '../godot/scene/main/node.cpp', '77'],
    ['report.md:12', 'docs/studies/report.md', '12'],
    ['a%20b.md', 'docs/studies/a b.md', null],
  ];
  for (const [href, expected, line] of cases) {
    const result = new URL(resolveDocumentLink(href, from), 'http://localhost:4178');
    assert.equal(result.searchParams.get('file'), expected);
    assert.equal(result.searchParams.get('line'), line);
  }
  assert.equal(resolveDocumentLink('https://example.com/docs', from), 'https://example.com/docs');
  assert.equal(resolveDocumentLink('#section', from), '#section');
  for (const name of ['README.md', 'PLAN.md']) {
    const result = new URL(resolveDocumentLink('../../../' + name, 'docs/studies/map/README.md'), 'http://localhost:4178');
    assert.equal(result.searchParams.get('file'), name);
  }
  for (const href of ['javascript:alert(1)', 'data:text/plain,hello', '//example.com/file', '/etc/passwd', 'C:\\private.md']) assert.equal(resolveDocumentLink(href, from), null);
});

test('file access is restricted to documentation and available sibling checkouts', async t => {
  const workspace = await mkdtemp(path.join(tmpdir(), 'faset-map-'));
  t.after(() => rm(workspace, { recursive: true, force: true }));
  const project = path.join(workspace, 'Faset_Engine');
  const files = {
    'Faset_Engine/README.md': 'project',
    'Faset_Engine/PLAN.md': 'plan',
    'Faset_Engine/docs/ARCHITECTURE.md': 'architecture',
    'Faset_Engine/docs/studies/report.md': 'report',
    'Faset_Engine/private.md': 'private',
    'Faset_Engine/docs/.private.md': 'hidden',
    'Faset_Engine/docs/node_modules/private.md': 'dependency',
    'Faset_Engine/docs/program.exe': 'binary',
    'outside.md': 'outside',
    'UnrealEngine/Engine/example.cpp': 'source',
    'UnrealEngine-other/example.cpp': 'wrong root',
  };
  for (const [name, text] of Object.entries(files)) {
    const target = path.join(workspace, name);
    await mkdir(path.dirname(target), { recursive: true });
    await writeFile(target, text);
  }
  assert.equal((await readDocument(project, 'README.md')).text, 'project');
  assert.equal((await readDocument(project, 'PLAN.md')).text, 'plan');
  assert.equal((await readDocument(project, 'docs/studies/../ARCHITECTURE.md')).path, 'docs/ARCHITECTURE.md');
  assert.equal((await readDocument(project, '../UnrealEngine/Engine/example.cpp')).path, '../UnrealEngine/Engine/example.cpp');
  for (const requested of ['private.md', '../outside.md', '../UnrealEngine-other/example.cpp', 'docs/.private.md', 'docs/node_modules/private.md', 'docs/program.exe', path.join(project, 'README.md')]) {
    await assert.rejects(readDocument(project, requested), { status: 403 });
  }
  await assert.rejects(readDocument(project, '../godot/missing.cpp'), { code: 'ENOENT' });
  await writeFile(path.join(project, 'docs/large.txt'), '');
  await truncate(path.join(project, 'docs/large.txt'), 8 * 1024 * 1024 + 1);
  await assert.rejects(readDocument(project, 'docs/large.txt'), { status: 403 });
  try {
    await symlink(path.join(workspace, 'outside.md'), path.join(project, 'docs/escape.md'));
  } catch (error) {
    if (error.code === 'EPERM') { t.diagnostic('Symlink case skipped: creating symlinks is not permitted on this host.'); return; }
    throw error;
  }
  await assert.rejects(readDocument(project, 'docs/escape.md'), { status: 403 });
  for (const name of ['README.md', 'PLAN.md']) {
    await rm(path.join(project, name));
    await symlink(path.join(workspace, 'outside.md'), path.join(project, name));
    await assert.rejects(readDocument(project, name), { status: 403 });
  }
});
