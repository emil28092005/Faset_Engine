import React, { useState } from 'react';
import { fileUrl } from './file-links.mjs';
export { fileUrl } from './file-links.mjs';

const theme = {
  text: { primary: 'var(--text)', secondary: 'var(--muted)', tertiary: 'var(--subtle)' },
  bg: { elevated: 'var(--surface)' }, fill: { secondary: 'var(--selected)' },
  stroke: { primary: 'var(--line)', secondary: 'var(--line)' },
  accent: { primary: 'var(--accent)' },
};
export const useHostTheme = () => theme;
export function useCanvasState(key: string, initial: string) {
  const [value, setValue] = useState(() => { try { return localStorage.getItem(key) ?? initial; } catch { return initial; } });
  return [value, (next: string) => { setValue(next); try { localStorage.setItem(key, next); } catch {} }] as const;
}
export const useCanvasAction = () => (action: any) => {
  if (action.type === 'openFile') window.location.assign(fileUrl(action.path, action.selection?.startLineNumber));
};
export function Stack({ gap = 12, style, ...props }: any) { return <div style={{ display: 'flex', flexDirection: 'column', gap, ...style }} {...props} />; }
export function Row({ gap = 12, justify, align = 'center', wrap, style, ...props }: any) { return <div style={{ display: 'flex', gap, justifyContent: justify, alignItems: align === 'start' ? 'flex-start' : align, flexWrap: wrap ? 'wrap' : undefined, ...style }} {...props} />; }
export function Grid({ columns, gap = 12, style, ...props }: any) { return <div style={{ display: 'grid', gridTemplateColumns: columns, gap, ...style }} {...props} />; }
export const H1 = (props: any) => <h1 {...props} />;
export const H2 = (props: any) => <h2 {...props} />;
export const H3 = (props: any) => <h3 {...props} />;
export const Text = ({ size, tone, weight, style, ...props }: any) => <p style={{ fontSize: size === 'small' ? 12 : undefined, color: tone === 'secondary' ? theme.text.secondary : tone === 'tertiary' ? theme.text.tertiary : undefined, fontWeight: weight === 'bold' ? 600 : undefined, ...style }} {...props} />;
export const Pill = ({ active, onClick, ...props }: any) => onClick ? <button className={'pill' + (active ? ' active' : '')} aria-pressed={!!active} onClick={onClick} {...props} /> : <span className="pill label" {...props} />;
export const Button = (props: any) => <button className="button" {...props} />;
export const Divider = () => <hr />;
export const Card = (props: any) => <section className="card" {...props} />;
export const CardHeader = (props: any) => <div className="card-header" {...props} />;
export const CardBody = (props: any) => <div className="card-body" {...props} />;
export const Table = ({ headers, rows }: any) => <div className="table-scroll"><table><thead><tr>{headers.map((h: string, i: number) => <th key={i}>{h}</th>)}</tr></thead><tbody>{rows.map((row: string[], i: number) => <tr key={i}>{row.map((cell, j) => <td key={j}>{cell}</td>)}</tr>)}</tbody></table></div>;
export function CollapsibleSection({ title, defaultOpen, children }: any) { return <details open={defaultOpen}><summary>{title}</summary><div className="details-content">{children}</div></details>; }
