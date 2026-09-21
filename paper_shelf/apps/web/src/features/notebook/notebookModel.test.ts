import { describe, expect, it } from "vitest";
import { deriveSearchText, eraseStrokeAt, eraseStrokesAt, moveObject, normalizePoint, reorderPages, sanitizeNotebookObjects, simplifyStroke, smoothStroke, type NotebookPage, type StrokeObject, type TextObject } from "./notebookModel";

describe("notebook model", () => {
  it("normalizes pressure-aware points", () => {
    expect(normalizePoint(50, 25, 100, 100, 0.7)).toEqual([0.5, 0.25, 0.7]);
  });

  it("simplifies only interior near-duplicate points", () => {
    expect(simplifyStroke([[0, 0, 1], [0.001, 0.001, 1], [0.5, 0.5, 1]], 0.01)).toEqual([[0, 0, 1], [0.5, 0.5, 1]]);
  });

  it("smooths a sharp middle point while preserving stroke endpoints", () => {
    expect(smoothStroke([[0, 0, 0.5], [0.2, 1, 0.5], [0.4, 0, 0.5]])).toEqual([[0, 0, 0.5], [0.2, 0.5, 0.5], [0.4, 0, 0.5]]);
  });

  it("moves objects, reorders pages, and derives typed search text", () => {
    const text: TextObject = { type: "text", id: "t", x: 0.1, y: 0.2, w: 0.3, h: 0.1, text: "hello", fontSize: 16 };
    const stroke: StrokeObject = { type: "stroke", id: "s", points: [[0, 0, 1]], width: 2 };
    const page: NotebookPage = { id: "p", pageIndex: 0, objects: [text, stroke] };
    expect(moveObject(text, 0.2, -0.5)).toMatchObject({ x: 0.3, y: 0 });
    expect(moveObject(stroke, 0.2, 0.3).points).toEqual([[0.2, 0.3, 1]]);
    expect(reorderPages([page, { ...page, id: "q", pageIndex: 1 }], 1, 0).map((item) => item.id)).toEqual(["q", "p"]);
    expect(deriveSearchText(page.objects)).toBe("hello");
  });

  it("drops malformed persisted notebook objects before rendering", () => {
    expect(sanitizeNotebookObjects([
      { type: "text", id: "valid", x: 0, y: 0, w: 1, h: 1, text: "Keep", fontSize: 16 },
      { type: "text", id: "bad", x: 0, y: 0, w: 1, h: 1, text: null, fontSize: 16 },
    ])).toHaveLength(1);
  });

  it("erases only the local stroke pixels and preserves the remaining segments", () => {
    const stroke: StrokeObject = { type: "stroke", id: "s", points: [[0, 0, 1], [0.5, 0, 1], [1, 0, 1]], width: 2 };
    expect(eraseStrokeAt(stroke, 0.5, 0, 0.05).map((item) => item.points)).toEqual([[[0, 0, 1]], [[1, 0, 1]]]);
  });

  it("erases strokes without deleting text notes", () => {
    const text: TextObject = { type: "text", id: "t", x: 0, y: 0, w: 0.5, h: 0.2, text: "Keep me", fontSize: 16 };
    const stroke: StrokeObject = { type: "stroke", id: "s", points: [[0.5, 0, 1]], width: 2 };
    expect(eraseStrokesAt([text, stroke], 0.5, 0, 0.05)).toEqual([text]);
  });
});
