import { useEffect, useRef } from "react";
import type { ScreenInputEvent } from "../shared/rpc";
import { MAC_KEY_CODES, modifiersOf, type ModifierState } from "./keys";

// This plugin typechecks without the DOM library. Declare only what this module uses.
interface DomRect { left: number; top: number; width: number; height: number }
interface DomMouseEvent extends ModifierState {
  clientX: number;
  clientY: number;
  button: number;
  detail: number;
  preventDefault(): void;
}
interface DomWheelEvent extends DomMouseEvent { deltaX: number; deltaY: number; deltaMode: number }
interface DomKeyboardEvent extends ModifierState { code: string; repeat: boolean; preventDefault(): void }
interface DomClipboardEvent { clipboardData: { getData(type: string): string } | null; preventDefault(): void }
interface DomEventTarget {
  addEventListener(type: string, listener: (event: never) => void, options?: { passive?: boolean }): void;
  removeEventListener(type: string, listener: (event: never) => void): void;
}
interface DomElement extends DomEventTarget {
  ownerDocument: DomEventTarget;
  tabIndex: number;
  style: Record<string, string>;
  focus(): void;
  getBoundingClientRect(): DomRect;
}

const BUTTONS = ["left", "middle", "right"] as const;

function isDomElement(node: unknown): node is DomElement {
  return typeof node === "object" && node !== null && "addEventListener" in node && "getBoundingClientRect" in node;
}

/**
 * Turns mouse, wheel, keyboard and paste events on the web/Electron client into host input.
 * `node` is the element that shows the remote screen; react-native-web refs resolve to DOM elements.
 */
export interface WebInputHandlers {
  /** Called after Cmd+C or Cmd+X is sent to the host. */
  onHostCopy(): void;
  /** Pastes local clipboard text on the host. */
  onPaste(text: string): void;
  /** Called when Cmd+V produced no paste event, so the caller can read the clipboard another way. */
  onPasteFallback(): void;
}

export function useWebInput(node: unknown, enabled: boolean, send: (event: ScreenInputEvent) => void, handlers: WebInputHandlers): void {
  const handlersRef = useRef(handlers);
  handlersRef.current = handlers;
  useEffect(() => {
    if (!enabled || !isDomElement(node)) return;
    const element = node;
    element.tabIndex = 0;
    element.style.outline = "none";
    element.style.cursor = "default";

    // During a drag the pointer may leave the picture; those positions continue onto the host's
    // neighbouring displays, so they are only kept within the range the host accepts.
    const at = (event: DomMouseEvent, dragging = false) => {
      const rect = element.getBoundingClientRect();
      const low = dragging ? -4 : 0;
      const high = dragging ? 5 : 1;
      return {
        x: Math.min(high, Math.max(low, (event.clientX - rect.left) / rect.width)),
        y: Math.min(high, Math.max(low, (event.clientY - rect.top) / rect.height)),
      };
    };
    const button = (event: DomMouseEvent) => BUTTONS[event.button] ?? "left";
    // While a button is down, moves and the release are read from the document so a drag
    // that leaves the view still ends on the host instead of leaving the button held.
    let pressed = 0;

    const onMouseDown = (event: DomMouseEvent) => {
      event.preventDefault();
      element.focus();
      pressed += 1;
      send({ type: "down", ...at(event), button: button(event), clickCount: Math.max(1, event.detail), modifiers: modifiersOf(event) });
    };
    const onMouseUp = (event: DomMouseEvent) => {
      if (pressed === 0) return;
      pressed -= 1;
      event.preventDefault();
      send({ type: "up", ...at(event, true), button: button(event), clickCount: Math.max(1, event.detail), modifiers: modifiersOf(event) });
    };
    const onElementMouseMove = (event: DomMouseEvent) => {
      if (pressed === 0) send({ type: "move", ...at(event) });
    };
    const onDocumentMouseMove = (event: DomMouseEvent) => {
      if (pressed > 0) send({ type: "move", ...at(event, true) });
    };
    const onWheel = (event: DomWheelEvent) => {
      event.preventDefault();
      // deltaMode 1 is lines, 2 is pages; the host scrolls in pixels.
      const factor = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? 400 : 1;
      send({ type: "scroll", ...at(event), dx: event.deltaX * factor, dy: event.deltaY * factor });
    };
    const onContextMenu = (event: DomMouseEvent) => event.preventDefault();
    // Cmd+V is not forwarded as a key. The browser's paste event carries this client's clipboard,
    // which is put on the host clipboard and pasted there. If no paste event arrives, fall back.
    let pasteTimer: ReturnType<typeof setTimeout> | undefined;
    const onKey = (down: boolean) => (event: DomKeyboardEvent) => {
      if (event.metaKey && event.code === "KeyV") {
        if (down && !event.repeat) {
          clearTimeout(pasteTimer);
          pasteTimer = setTimeout(() => handlersRef.current.onPasteFallback(), 200);
        }
        return;
      }
      const keyCode = MAC_KEY_CODES[event.code];
      event.preventDefault();
      if (keyCode === undefined) return;
      send({ type: "key", keyCode, down, modifiers: modifiersOf(event) });
      if (down && event.metaKey && (event.code === "KeyC" || event.code === "KeyX")) handlersRef.current.onHostCopy();
    };
    const onKeyDown = onKey(true);
    const onKeyUp = onKey(false);
    const onPaste = (event: DomClipboardEvent) => {
      clearTimeout(pasteTimer);
      const text = event.clipboardData?.getData("text/plain");
      event.preventDefault();
      if (text) handlersRef.current.onPaste(text);
    };

    const listeners: [string, (event: never) => void, { passive?: boolean }?][] = [
      ["mousedown", onMouseDown],
      ["mousemove", onElementMouseMove],
      ["wheel", onWheel, { passive: false }],
      ["contextmenu", onContextMenu],
      ["keydown", onKeyDown],
      ["keyup", onKeyUp],
      ["paste", onPaste],
    ];
    const documentListeners: [string, (event: never) => void][] = [
      ["mousemove", onDocumentMouseMove],
      ["mouseup", onMouseUp],
    ];
    const doc = element.ownerDocument;
    for (const [type, listener, options] of listeners) element.addEventListener(type, listener, options);
    for (const [type, listener] of documentListeners) doc.addEventListener(type, listener);
    return () => {
      clearTimeout(pasteTimer);
      for (const [type, listener] of listeners) element.removeEventListener(type, listener);
      for (const [type, listener] of documentListeners) doc.removeEventListener(type, listener);
    };
  }, [node, enabled, send]);
}
