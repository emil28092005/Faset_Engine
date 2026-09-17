import React, { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import Markdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import EngineResearch from './research';
import { resolveDocumentLink } from './file-links.mjs';
import './style.css';

function sourcePositions() {
  return (tree: any) => {
    const visit = (node: any) => {
      if (node.type === 'element' && node.position?.start?.line) node.properties = { ...node.properties, 'data-source-line': node.position.start.line };
      node.children?.forEach(visit);
    };
    visit(tree);
  };
}

function DocumentViewer({ path, line }: {path: string, line: number}) {
  const [text, setText] = useState<string | null>(null);
  const [documentPath, setDocumentPath] = useState(path);
  const [error, setError] = useState('');
  useEffect(() => {
    setText(null);
    setError('');
    const controller = new AbortController();
    fetch('/api/file?' + new URLSearchParams({ path }), {signal: controller.signal})
      .then(async r => {
        if (!r.ok) {
          const optionalSource = /^\.\.\/(UnrealEngine|godot|UnityCsReference|blender-source)\//.test(path);
          throw new Error(optionalSource && r.status === 404
            ? 'Локальный исходник не найден. Sibling checkout движка необязателен и не входит в Faset. Вернитесь к карте или отчёту и используйте upstream-ссылку; для Unreal Engine нужен доступ Epic.'
            : 'Файл недоступен (' + r.status + ')');
        }
        return r.json();
      })
      .then(data => { setDocumentPath(data.path); setText(data.text); }).catch(e => { if (e.name !== 'AbortError') setError(e.message); });
    return () => controller.abort();
  }, [path]);
  useEffect(() => {
    if (text === null || !line) return;
    const frame = requestAnimationFrame(() => {
      const exact = document.getElementById('L' + line);
      const nearest = Array.from(document.querySelectorAll<HTMLElement>('[data-source-line]')).filter(node => Number(node.dataset.sourceLine) <= line).sort((a, b) => Number(b.dataset.sourceLine) - Number(a.dataset.sourceLine))[0];
      (exact || nearest)?.scrollIntoView({block: 'center'});
    });
    return () => cancelAnimationFrame(frame);
  }, [text, line]);
  const markdown = /\.md$/i.test(documentPath);
  return <main className={'document' + (markdown ? '' : ' source')}>
    <header className="document-header"><a href="/">← Карта исследования</a><span>{documentPath}</span></header>
    {error && <p role="alert">{error}</p>}
    {text === null && !error && <p>Загрузка документа…</p>}
    {text !== null && (markdown ? <article className="markdown"><Markdown remarkPlugins={[remarkGfm]} rehypePlugins={[sourcePositions]} urlTransform={(url, key) => key === 'href' ? resolveDocumentLink(url, documentPath) || '' : ''} components={{ a: ({href, children}) => {
      if (!href) return <span>{children}</span>;
      if (/^https?:/.test(href)) return <a href={href} target="_blank" rel="noreferrer">{children}</a>;
      if (href.startsWith('#')) return <a href={href}>{children}</a>;
      return <a href={href}>{children}</a>;
    }}}>{text}</Markdown></article> : <pre className="source-code"><code>{text.split('\n').map((s, i) => <div id={'L' + (i + 1)} className={line === i + 1 ? 'highlight-line' : ''} key={i}><a className="line-number" href={'#L' + (i + 1)}>{i + 1}</a><span>{s || ' '}</span></div>)}</code></pre>)}
  </main>;
}
const params = new URLSearchParams(location.search);
createRoot(document.getElementById('root')!).render(params.has('file') ? <DocumentViewer path={params.get('file')!} line={Number(params.get('line')) || 0} /> : <EngineResearch />);
