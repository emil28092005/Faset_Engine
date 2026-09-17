export function fileUrl(path, line, fragment) {
  const query = new URLSearchParams({ file: path });
  if (line) query.set('line', String(line));
  return '/?' + query + (fragment ? '#' + fragment : '');
}

export function resolveDocumentLink(href, documentPath) {
  if (/^https?:\/\//i.test(href) || href.startsWith('#')) return href;
  const hash = href.indexOf('#');
  const fragment = hash < 0 ? '' : href.slice(hash + 1);
  const reference = hash < 0 ? href : href.slice(0, hash);
  const match = reference.match(/^(.*?)(?::([1-9]\d*))?$/);
  let target;
  try { target = decodeURIComponent(match[1]); } catch { return null; }
  if (!target || /^[a-z][a-z\d+.-]*:/i.test(target) || /^[\\/]/.test(target) || target.includes('\\')) return null;
  const parts = documentPath.split('/').slice(0, -1);
  for (const part of target.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..' && parts.length && parts.at(-1) !== '..') parts.pop();
    else parts.push(part);
  }
  const lineFragment = fragment.match(/^L([1-9]\d*)$/);
  return fileUrl(parts.join('/'), Number(lineFragment?.[1] || match[2]) || undefined, lineFragment ? undefined : fragment);
}
