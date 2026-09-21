import { useEffect, useRef, useState } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import { Alert, Box, Button, ButtonGroup, Stack, Typography } from "@mui/material";
import { listNotebookPages, saveNotebookPage, type StoredNotebookPage } from "../../lib/api";
import { deriveSearchText, eraseStrokesAt, moveObject, normalizePoint, reorderPages, sanitizeNotebookObjects, simplifyStroke, smoothStroke, type NotebookObject, type StrokeObject, type TextObject } from "./notebookModel";
import { deleteNotebookPage, setNotebookPageIndex } from "../../lib/api";

const PAGE_WIDTH = 1000;
const PAGE_HEIGHT = 1400;

export function Notebook({ paperId }: { paperId: string }) {
  const [pages, setPages] = useState<StoredNotebookPage[]>([]);
  const [pageIndex, setPageIndex] = useState(0);
  const [objects, setObjects] = useState<NotebookObject[]>([]);
  const [, setVersion] = useState(0);
  const [mode, setMode] = useState<"draw" | "text" | "erase" | "select">("select");
  const [message, setMessage] = useState("");
  const [history, setHistory] = useState<NotebookObject[][]>([]);
  const [future, setFuture] = useState<NotebookObject[][]>([]);
  const [draftText, setDraftText] = useState<{ x: number; y: number } | null>(null);
  const [draftValue, setDraftValue] = useState("");
  const draftInput = useRef<HTMLTextAreaElement>(null);
  const versionRef = useRef(0);
  const [conflict, setConflict] = useState(false);
  const activeStroke = useRef<StrokeObject | null>(null);
  const activeErase = useRef<{ original: NotebookObject[]; changed: boolean } | null>(null);
  const draggedObject = useRef<{ id: string; startX: number; startY: number; original: NotebookObject[] } | null>(null);

  useEffect(() => {
    void listNotebookPages(paperId).then((loaded) => {
      const next = loaded.length ? loaded.map((page) => ({ ...page, objects: sanitizeNotebookObjects(page.objects) })) : [{ id: crypto.randomUUID(), page_index: 0, objects: [], search_text: "", version: 0 }];
      setPages(next); setObjects(next[0].objects); setVersion(next[0].version); versionRef.current = next[0].version;
    }).catch((error: Error) => setMessage(error.message));
  }, [paperId]);

  useEffect(() => {
    if (!pages.length) return;
    const timer = window.setTimeout(() => {
      void saveNotebookPage(paperId, pageIndex, objects, versionRef.current).then((saved) => { versionRef.current = saved.version; setVersion(saved.version); setConflict(false); setPages((current) => current.map((page) => page.page_index === pageIndex ? saved : page)); }).catch((error: Error) => { setMessage(error.message); setConflict(error.message.includes("Notebook changed elsewhere")); });
    }, 750);
    return () => window.clearTimeout(timer);
  }, [paperId, pageIndex, objects, pages.length]);
  useEffect(() => { if (draftText) draftInput.current?.focus(); }, [draftText]);

  function changeObjects(next: NotebookObject[]) { setHistory((current) => [...current, objects]); setFuture([]); setObjects(next); }
  function pointerDown(event: ReactPointerEvent<HTMLDivElement>) {
    if (mode === "text") {
      placeText(event);
      return;
    }
    if (mode === "erase") {
      event.currentTarget.setPointerCapture(event.pointerId);
      activeErase.current = { original: objects, changed: false };
      eraseAt(event);
      return;
    }
    if (mode !== "draw") return;
    event.currentTarget.setPointerCapture(event.pointerId);
    const rect = event.currentTarget.getBoundingClientRect();
    activeStroke.current = { type: "stroke", id: crypto.randomUUID(), points: [normalizePoint(event.clientX - rect.left, event.clientY - rect.top, rect.width, rect.height, event.pressure || 0.5)], width: 2.2 };
  }
  function pointerMove(event: ReactPointerEvent<HTMLDivElement>) {
    if (draggedObject.current) {
      const rect = event.currentTarget.getBoundingClientRect();
      const dx = (event.clientX - draggedObject.current.startX) / rect.width;
      const dy = (event.clientY - draggedObject.current.startY) / rect.height;
      setObjects(draggedObject.current.original.map((object) => object.id === draggedObject.current?.id ? moveObject(object, dx, dy) : object));
      return;
    }
    if (activeErase.current) {
      eraseAt(event);
      return;
    }
    if (!activeStroke.current) return;
    const rect = event.currentTarget.getBoundingClientRect();
    const stroke = activeStroke.current;
    if (!stroke) return;
    stroke.points.push(normalizePoint(event.clientX - rect.left, event.clientY - rect.top, rect.width, rect.height, event.pressure || 0.5));
    const snapshot = { ...stroke, points: [...stroke.points] };
    setObjects((current) => [...current.filter((object) => object.id !== snapshot.id), snapshot]);
  }
  function pointerUp() {
    if (draggedObject.current) {
      setHistory((current) => [...current, draggedObject.current!.original]); setFuture([]); draggedObject.current = null; return;
    }
    if (activeErase.current) {
      if (activeErase.current.changed) {
        setHistory((current) => [...current, activeErase.current!.original]); setFuture([]);
      }
      activeErase.current = null;
      return;
    }
    if (!activeStroke.current) return;
    const stroke = { ...activeStroke.current, points: smoothStroke(simplifyStroke(activeStroke.current.points)) };
    setHistory((current) => [...current, objects]); setFuture([]); setObjects((current) => [...current.filter((object) => object.id !== stroke.id), stroke]); activeStroke.current = null;
  }
  function commitText() {
    if (!draftText) return;
    const text = draftValue.trim();
    if (text) changeObjects([...objects, { type: "text", id: crypto.randomUUID(), ...draftText, w: 0.4, h: 0.08, text, fontSize: 16 }]);
    setDraftText(null);
    setDraftValue("");
    setMode("text");
  }
  function placeText(event: ReactPointerEvent<HTMLDivElement>) {
    const rect = event.currentTarget.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    setDraftText({ x: Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width)), y: Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height)) });
    setDraftValue("");
  }
  function eraseAt(event: ReactPointerEvent<HTMLDivElement>) {
    const rect = event.currentTarget.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const x = (event.clientX - rect.left) / rect.width; const y = (event.clientY - rect.top) / rect.height;
    setObjects((current) => {
      const next = eraseStrokesAt(current, x, y);
      if (next.every((object, index) => object === current[index])) return current;
      if (activeErase.current) activeErase.current.changed = true;
      return next;
    });
  }
  function removeObject(id: string) { changeObjects(objects.filter((object) => object.id !== id)); }
  function armText() {
    setMode("text");
    setDraftText((current) => current ?? { x: 0.08, y: 0.08 });
    setDraftValue("");
  }
  function undo() { const previous = history.at(-1); if (!previous) return; setFuture((current) => [...current, objects]); setHistory((current) => current.slice(0, -1)); setObjects(previous); }
  function redo() { const next = future.at(-1); if (!next) return; setHistory((current) => [...current, objects]); setFuture((current) => current.slice(0, -1)); setObjects(next); }

  function startObjectDrag(event: ReactPointerEvent<HTMLElement | SVGElement>, id: string) {
    if (mode !== "select") return;
    event.stopPropagation();
    draggedObject.current = { id, startX: event.clientX, startY: event.clientY, original: objects };
    (event.currentTarget as HTMLElement | SVGElement).setPointerCapture(event.pointerId);
  }

  function resizeText(event: ReactPointerEvent<HTMLDivElement>, id: string) {
    if (mode !== "select") return;
    const parent = event.currentTarget.parentElement?.getBoundingClientRect();
    const rect = event.currentTarget.getBoundingClientRect();
    if (!parent || !parent.width || !parent.height) return;
    setObjects((current) => current.map((object) => object.id === id && object.type === "text" ? { ...object, w: Math.min(1 - object.x, rect.width / parent.width), h: Math.min(1 - object.y, rect.height / parent.height) } : object));
  }

  function reloadLatest() {
    void listNotebookPages(paperId).then((loaded) => { const latest = loaded.find((page) => page.page_index === pageIndex); if (latest) { setObjects(sanitizeNotebookObjects(latest.objects)); setVersion(latest.version); versionRef.current = latest.version; setConflict(false); setMessage(""); } }).catch((error: Error) => setMessage(error.message));
  }
  function keepAsNewPage() {
    const next = { id: crypto.randomUUID(), page_index: pages.length, objects, search_text: deriveSearchText(objects), version: 0 };
    setPages([...pages, next]); setPageIndex(next.page_index); setVersion(0); versionRef.current = 0; setConflict(false); setMessage("");
  }
  function selectPage(next: StoredNotebookPage) { setPageIndex(next.page_index); setObjects(sanitizeNotebookObjects(next.objects)); setVersion(next.version); versionRef.current = next.version; setHistory([]); setFuture([]); setConflict(false); }
  async function movePage(delta: number) {
    const from = pages.findIndex((page) => page.page_index === pageIndex);
    const to = Math.max(0, Math.min(pages.length - 1, from + delta));
    if (from < 0 || from === to) return;
    const ordered = reorderPages(pages.map((page) => ({ id: page.id, pageIndex: page.page_index, objects: page.objects })), from, to);
    const next = ordered.map((page) => ({ ...pages.find((current) => current.id === page.id)!, page_index: page.pageIndex }));
    try {
      await Promise.all(pages.map((page, index) => setNotebookPageIndex(page.id, pages.length + 10000 + index)));
      await Promise.all(next.map((page) => setNotebookPageIndex(page.id, page.page_index)));
      setPages(next); selectPage(next[to]);
    } catch (error) { setMessage((error as Error).message); }
  }
  async function removePage() {
    if (pages.length === 1) return;
    const current = pages.find((page) => page.page_index === pageIndex);
    if (!current) return;
    try {
      await deleteNotebookPage(current.id);
      const remaining = pages.filter((page) => page.id !== current.id).map((page, index) => ({ ...page, page_index: index }));
      await Promise.all(remaining.map((page, index) => setNotebookPageIndex(page.id, remaining.length + 10000 + index)));
      await Promise.all(remaining.map((page) => setNotebookPageIndex(page.id, page.page_index)));
      setPages(remaining); selectPage(remaining[Math.min(pageIndex, remaining.length - 1)]);
    } catch (error) { setMessage((error as Error).message); }
  }

  return <Box component="section"><Stack direction="row" sx={{ mb: 1.5, justifyContent: "space-between", alignItems: "center" }}><Typography component="h2" variant="h2">Notebook</Typography><Typography variant="caption" color="text.secondary">Page {pageIndex + 1} of {Math.max(1, pages.length)}</Typography></Stack>
    <Stack direction="row" spacing={1} useFlexGap sx={{ mb: 1.5, flexWrap: "wrap" }}><ButtonGroup size="small"><Button variant={mode === "select" ? "contained" : "outlined"} onClick={() => setMode("select")}>Select</Button><Button variant={mode === "draw" ? "contained" : "outlined"} onClick={() => setMode("draw")}>Pen</Button><Button variant={mode === "text" ? "contained" : "outlined"} onClick={armText}>Text</Button><Button variant={mode === "erase" ? "contained" : "outlined"} onClick={() => setMode("erase")}>Eraser</Button></ButtonGroup><ButtonGroup size="small"><Button onClick={undo} disabled={!history.length}>Undo</Button><Button onClick={redo} disabled={!future.length}>Redo</Button><Button onClick={() => { const next = { id: crypto.randomUUID(), page_index: pages.length, objects: [], search_text: "", version: 0 }; setPages([...pages, next]); selectPage(next); }}>Add page</Button></ButtonGroup><Button size="small" onClick={() => void movePage(-1)} disabled={!pageIndex}>Move up</Button><Button size="small" onClick={() => void movePage(1)} disabled={pageIndex >= pages.length - 1}>Move down</Button><Button size="small" color="error" onClick={() => void removePage()} disabled={pages.length < 2}>Delete</Button></Stack>
    {mode === "text" && <Typography variant="caption" color="primary.main" sx={{ display: "block", mb: 1 }}>Click anywhere on the page to type a note.</Typography>}
    {message && <Alert severity={conflict ? "warning" : "error"} sx={{ mb: 1.5 }}>{message}{conflict && <Stack direction="row" spacing={1} sx={{ mt: 1 }}><Button size="small" onClick={reloadLatest}>Reload latest</Button><Button size="small" variant="outlined" onClick={keepAsNewPage}>Keep my copy as new page</Button></Stack>}</Alert>}
    <Stack direction={{ xs: "column", sm: "row" }} spacing={1.5}><Stack direction="column" spacing={0.75} sx={{ overflowY: "auto", maxWidth: 92, maxHeight: 620, flexShrink: 0 }}>{pages.map((page) => <Button size="small" key={page.id} variant={page.page_index === pageIndex ? "contained" : "outlined"} onClick={() => selectPage(page)} sx={{ flexShrink: 0 }}>Page {page.page_index + 1}</Button>)}</Stack>
      <Box className="notebook-canvas" onPointerDown={pointerDown} onPointerMove={pointerMove} onPointerUp={pointerUp} onPointerCancel={pointerUp} sx={{ position: "relative", flex: 1, minWidth: 0, width: "100%", aspectRatio: `${PAGE_WIDTH}/${PAGE_HEIGHT}`, border: 1, borderColor: "divider", touchAction: mode === "draw" || mode === "text" || mode === "erase" ? "none" : "pan-y", overflow: "hidden", cursor: mode === "text" ? "text" : mode === "erase" ? "crosshair" : "default", "--notebook-bg": "#fff", "--notebook-dot": "#bfd0d8" }}>
        <svg viewBox={`0 0 ${PAGE_WIDTH} ${PAGE_HEIGHT}`} width="100%" height="100%">{objects.filter((object): object is StrokeObject => object.type === "stroke").map((stroke) => stroke.points.length === 1 ? <circle key={stroke.id} onPointerDown={(event) => startObjectDrag(event, stroke.id)} cx={stroke.points[0][0] * PAGE_WIDTH} cy={stroke.points[0][1] * PAGE_HEIGHT} r={stroke.width / 2} fill="black" /> : <polyline key={stroke.id} onPointerDown={(event) => startObjectDrag(event, stroke.id)} points={stroke.points.map(([x, y]) => `${x * PAGE_WIDTH},${y * PAGE_HEIGHT}`).join(" ")} fill="none" stroke="black" strokeWidth={stroke.width} strokeLinecap="round" strokeLinejoin="round" />)}</svg>
        {objects.filter((object): object is TextObject => object.type === "text").map((text) => <div key={text.id} onPointerDown={(event) => startObjectDrag(event, text.id)} onPointerUp={(event) => resizeText(event, text.id)} style={{ position: "absolute", left: `${text.x * 100}%`, top: `${text.y * 100}%`, width: `${text.w * 100}%`, minHeight: `${text.h * 100}%`, paddingRight: mode === "select" ? 24 : 0, fontSize: text.fontSize, resize: mode === "select" ? "both" : "none", overflow: "auto", cursor: mode === "select" ? "move" : "default" }}><span>{text.text}</span>{mode === "select" && <button type="button" aria-label="Delete note" onPointerDown={(event) => event.stopPropagation()} onClick={(event) => { event.stopPropagation(); removeObject(text.id); }} style={{ position: "absolute", top: 2, right: 2, width: 20, height: 20, padding: 0, border: 0, borderRadius: "50%", background: "#d95d5d", color: "white", cursor: "pointer", lineHeight: 1 }}>×</button>}</div>)}
        {draftText && <textarea ref={draftInput} autoFocus value={draftValue} onChange={(event) => setDraftValue(event.target.value)} onBlur={() => { if (draftValue.trim()) commitText(); }} onKeyDown={(event) => { if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) { event.preventDefault(); commitText(); } if (event.key === "Escape") { setDraftText(null); setDraftValue(""); setMode("select"); } }} onPointerDown={(event) => event.stopPropagation()} onClick={(event) => event.stopPropagation()} placeholder="Type a note…" style={{ position: "absolute", left: `${draftText.x * 100}%`, top: `${draftText.y * 100}%`, width: "40%", minHeight: "8%", zIndex: 2, padding: 8, border: "1px solid #75bda7", borderRadius: 6, background: "white", color: "#17211d", font: "16px sans-serif", resize: "both" }} />}
      </Box>
    </Stack><Typography variant="caption" color="text.secondary" sx={{ display: "block", mt: 1.5 }}>{deriveSearchText(objects) || "Your notes are searchable from Library."}</Typography>
  </Box>;
}
