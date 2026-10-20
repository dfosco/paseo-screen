/** Layout of the remote picture inside the viewer, for the touch viewer's rotation, zoom and pan. */

export interface Size {
  width: number;
  height: number;
}

export interface View2D {
  /** Picture size before zoom, in points, measured along the picture's own axes. */
  content: Size;
  rotated: boolean;
  zoom: number;
  /** Pan offset in unzoomed picture points, along the picture's own axes. */
  panX: number;
  panY: number;
}

export const MAX_ZOOM = 5;

/** Largest picture of `aspect` (width / height) that fits `stage`, turned 90° when `rotated`. */
export function fitContent(stage: Size, aspect: number, rotated: boolean): Size {
  if (stage.width <= 0 || stage.height <= 0) return { width: 0, height: 0 };
  // Rotated, the picture's width runs along the stage's height.
  const along = rotated ? stage.height : stage.width;
  const across = rotated ? stage.width : stage.height;
  const width = Math.floor(Math.min(along, across * aspect));
  return { width, height: Math.floor(width / aspect) };
}

/** Keeps the zoomed picture covering the stage instead of drifting off it. */
export function clampPan(view: View2D, stage: Size): View2D {
  // Visible stage extent along the picture's axes, in unzoomed picture points.
  const visibleWidth = (view.rotated ? stage.height : stage.width) / view.zoom;
  const visibleHeight = (view.rotated ? stage.width : stage.height) / view.zoom;
  const limitX = Math.max(0, (view.content.width - visibleWidth) / 2);
  const limitY = Math.max(0, (view.content.height - visibleHeight) / 2);
  return {
    ...view,
    panX: Math.min(limitX, Math.max(-limitX, view.panX)),
    panY: Math.min(limitY, Math.max(-limitY, view.panY)),
  };
}

/** Turns a stage-space movement into the same movement along the picture's axes. */
export function toContentDelta(dx: number, dy: number, rotated: boolean): { dx: number; dy: number } {
  // The picture is turned 90° clockwise: its +x points down the stage and its +y points left.
  return rotated ? { dx: dy, dy: -dx } : { dx, dy };
}

/**
 * Maps a point on the stage to normalized 0...1 coordinates on the remote screen, or null when it
 * falls outside the picture. Inverts the transform [rotate, scale(zoom), translate(pan)] about the stage center.
 */
export function toScreenPoint(stageX: number, stageY: number, stage: Size, view: View2D): { x: number; y: number } | null {
  if (view.content.width === 0 || view.content.height === 0) return null;
  const delta = toContentDelta(stageX - stage.width / 2, stageY - stage.height / 2, view.rotated);
  const px = delta.dx / view.zoom - view.panX;
  const py = delta.dy / view.zoom - view.panY;
  const x = px / view.content.width + 0.5;
  const y = py / view.content.height + 0.5;
  if (x < 0 || x > 1 || y < 0 || y > 1) return null;
  return { x, y };
}
