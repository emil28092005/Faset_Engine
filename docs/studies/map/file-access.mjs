import { readFile, realpath, stat } from 'node:fs/promises';
import path from 'node:path';

const sourceFolders = ['UnrealEngine', 'godot', 'UnityCsReference', 'blender-source'];
const extensions = /\.(md|json|version|cpp|h|hpp|c|cc|cs|usf|ush|glsl|hlsl|py|txt|inl|xml|gd|godot|sln|cmake)$/i;
const denied = () => Object.assign(new Error('File not allowed'), { status: 403 });
const contained = (target, root) => target.startsWith(root + path.sep);

export async function readDocument(projectRoot, requestedPath) {
  if (!requestedPath || path.isAbsolute(requestedPath) || /^[a-z]:/i.test(requestedPath) || requestedPath.includes('\\')) throw denied();
  if (requestedPath.split('/').some(part => (part.startsWith('.') && part !== '.' && part !== '..') || part === 'node_modules')) throw denied();
  const realProject = await realpath(projectRoot);
  const target = await realpath(path.resolve(projectRoot, requestedPath));
  const roots = [path.join(projectRoot, 'docs'), ...sourceFolders.map(name => path.resolve(projectRoot, '..', name))];
  const realRoots = await Promise.all(roots.map(root => realpath(root).catch(() => null)));
  if (realRoots[0] && !contained(realRoots[0], realProject)) realRoots[0] = null;
  const projectDocuments = ['README.md', 'PLAN.md'].map(name => path.join(realProject, name));
  if ((!realRoots.some(root => root && contained(target, root)) && !projectDocuments.includes(target))
      || (!extensions.test(target) && path.basename(target) !== 'SConstruct')
      || target.split(path.sep).some(part => part.startsWith('.') || part === 'node_modules')) throw denied();
  const info = await stat(target);
  if (!info.isFile() || info.size > 8 * 1024 * 1024) throw denied();
  return { path: path.relative(realProject, target).split(path.sep).join('/'), text: await readFile(target, 'utf8') };
}
