export type StrokePoint = [number, number, number];
export type StrokeObject = { type: "stroke"; id: string; points: StrokePoint[]; width: number };
export type TextObject = { type: "text"; id: string; x: number; y: number; w: number; h: number; text: string; fontSize: number };
export type NotebookObject = StrokeObject | TextObject;
export type NotebookPage = { id: string; pageIndex: number; objects: NotebookObject[] };

export function sanitizeNotebookObjects(value: unknown): NotebookObject[] {
  if (!Array.isArray(value)) return [];
  return value.filter((object): object is NotebookObject => {
    if (!object || typeof object !== "object" || typeof (object as { id?: unknown }).id !== "string") return false;
    if ((object as { type?: unknown }).type === "text") {
      const text = object as Partial<TextObject>;
      return typeof text.text === "string" && [text.x, text.y, text.w, text.h, text.fontSize].every((item) => typeof item === "number" && Number.isFinite(item));
    }
    const stroke = object as Partial<StrokeObject>;
    return stroke.type === "stroke" && typeof stroke.width === "number" && Number.isFinite(stroke.width) && Array.isArray(stroke.points) && stroke.points.every((point) => Array.isArray(point) && point.length === 3 && point.every((item) => typeof item === "number" && Number.isFinite(item)));
  });
}

export function normalizePoint(x: number, y: number, width: number, height: number, pressure = 0.5): StrokePoint {
  if (width <= 0 || height <= 0) throw new Error("page dimensions must be positive");
  return [Math.max(0, Math.min(1, x / width)), Math.max(0, Math.min(1, y / height)), Math.max(0, Math.min(1, pressure))];
}

export function simplifyStroke(points: StrokePoint[], tolerance = 0.002): StrokePoint[] {
  if (points.length <= 2) return points;
  const result = [points[0]];
  for (const point of points.slice(1, -1)) {
    const previous = result[result.length - 1];
    if (Math.hypot(point[0] - previous[0], point[1] - previous[1]) >= tolerance) result.push(point);
  }
  result.push(points[points.length - 1]);
  return result;
}

export function smoothStroke(points: StrokePoint[]): StrokePoint[] {
  if (points.length < 3) return points;
  return points.map((point, index) => {
    if (index === 0 || index === points.length - 1) return point;
    const previous = points[index - 1];
    const next = points[index + 1];
    return [
      (previous[0] + 2 * point[0] + next[0]) / 4,
      (previous[1] + 2 * point[1] + next[1]) / 4,
      (previous[2] + 2 * point[2] + next[2]) / 4,
    ];
  });
}

export function eraseStrokeAt(stroke: StrokeObject, x: number, y: number, radius = 0.012): StrokeObject[] {
  const hit = stroke.points.map((point) => Math.hypot(point[0] - x, point[1] - y) <= radius);
  if (!hit.some(Boolean)) return [stroke];
  const segments: StrokePoint[][] = [];
  let segment: StrokePoint[] = [];
  stroke.points.forEach((point, index) => {
    if (hit[index]) {
      if (segment.length) segments.push(segment);
      segment = [];
    } else segment.push(point);
  });
  if (segment.length) segments.push(segment);
  return segments.map((points, index) => ({ ...stroke, id: index ? `${stroke.id}-${index}` : stroke.id, points }));
}

export function eraseStrokesAt(objects: NotebookObject[], x: number, y: number, radius = 0.012): NotebookObject[] {
  return objects.flatMap((object): NotebookObject[] => object.type === "stroke" ? eraseStrokeAt(object, x, y, radius) : [object]);
}

export function moveObject<T extends NotebookObject>(object: T, dx: number, dy: number): T {
  const round = (value: number) => Math.round(value * 1_000_000) / 1_000_000;
  if (object.type === "stroke") {
    return { ...object, points: object.points.map(([x, y, pressure]) => [round(Math.max(0, Math.min(1, x + dx))), round(Math.max(0, Math.min(1, y + dy))), pressure]) } as T;
  }
  return { ...object, x: round(Math.max(0, Math.min(1 - object.w, object.x + dx))), y: round(Math.max(0, Math.min(1 - object.h, object.y + dy))) } as T;
}

export function reorderPages(pages: NotebookPage[], from: number, to: number): NotebookPage[] {
  const result = [...pages];
  const [page] = result.splice(from, 1);
  result.splice(to, 0, page);
  return result.map((item, pageIndex) => ({ ...item, pageIndex }));
}

export function deriveSearchText(objects: NotebookObject[]): string {
  return objects.filter((object): object is TextObject => object.type === "text").map((object) => object.text.trim()).filter(Boolean).join(" ");
}
