import { Platform } from "react-native";
import type { ScreenModifier } from "../shared/rpc";

// This plugin typechecks without the DOM library. Declare only what this module uses.
declare const navigator: { platform?: string; userAgent?: string } | undefined;

/** KeyboardEvent.code to macOS virtual key code (Carbon kVK_* values). Physical positions, so the host layout decides the character. */
export const MAC_KEY_CODES: Record<string, number> = {
  KeyA: 0, KeyS: 1, KeyD: 2, KeyF: 3, KeyH: 4, KeyG: 5, KeyZ: 6, KeyX: 7, KeyC: 8, KeyV: 9,
  IntlBackslash: 10, KeyB: 11, KeyQ: 12, KeyW: 13, KeyE: 14, KeyR: 15, KeyY: 16, KeyT: 17,
  Digit1: 18, Digit2: 19, Digit3: 20, Digit4: 21, Digit6: 22, Digit5: 23, Equal: 24, Digit9: 25,
  Digit7: 26, Minus: 27, Digit8: 28, Digit0: 29, BracketRight: 30, KeyO: 31, KeyU: 32,
  BracketLeft: 33, KeyI: 34, KeyP: 35, Enter: 36, KeyL: 37, KeyJ: 38, Quote: 39, KeyK: 40,
  Semicolon: 41, Backslash: 42, Comma: 43, Slash: 44, KeyN: 45, KeyM: 46, Period: 47, Tab: 48,
  Space: 49, Backquote: 50, Backspace: 51, Escape: 53,
  NumpadDecimal: 65, NumpadMultiply: 67, NumpadAdd: 69, NumLock: 71, NumpadDivide: 75,
  NumpadEnter: 76, NumpadSubtract: 78, NumpadEqual: 81, Numpad0: 82, Numpad1: 83, Numpad2: 84,
  Numpad3: 85, Numpad4: 86, Numpad5: 87, Numpad6: 88, Numpad7: 89, Numpad8: 91, Numpad9: 92,
  F1: 122, F2: 120, F3: 99, F4: 118, F5: 96, F6: 97, F7: 98, F8: 100, F9: 101, F10: 109,
  F11: 103, F12: 111, F13: 105, F14: 107, F15: 113, F16: 106, F17: 64, F18: 79, F19: 80, F20: 90,
  Insert: 114, Help: 114, Home: 115, PageUp: 116, Delete: 117, End: 119, PageDown: 121,
  ArrowLeft: 123, ArrowRight: 124, ArrowDown: 125, ArrowUp: 126,
};

export const KEY = {
  enter: 36,
  tab: 48,
  space: 49,
  backspace: 51,
  escape: 53,
  /** Left Command on a macOS host, left Super on a Linux host. */
  command: 55,
  f4: 118,
  left: 123,
  right: 124,
  down: 125,
  up: 126,
} as const;

export interface ModifierState {
  metaKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
  ctrlKey: boolean;
}

/** True when this client is a Mac, iPhone or iPad, where Command is the shortcut key. */
export function isAppleClient(): boolean {
  if (Platform.OS === "ios") return true;
  if (typeof navigator === "undefined") return false;
  return /^(Mac|iPhone|iPad)/.test(navigator.platform ?? "") || /Macintosh|iPhone|iPad/.test(navigator.userAgent ?? "");
}

/**
 * How this client's keys become host shortcuts. A Mac client's Command key is the Mac host's Command,
 * but on a Linux host it is sent as Ctrl, the shortcut key there. Other clients' Ctrl stays Ctrl, and
 * their Super (Windows) key is sent as `cmd`, which is Command on a Mac host and Super on a Linux host.
 */
export interface KeyMapping {
  commandAsCtrl: boolean;
  /** Whether a paste or copy shortcut is held: Command, or Ctrl when a non-Apple client controls a Linux host. */
  shortcut(event: ModifierState): boolean;
  /**
   * Send each key press as a press and release. An X server repeats a key held through XTest, so a
   * key-up that arrives late or never would keep typing; this client's own key repeat still repeats keys.
   */
  tapKeys: boolean;
}

export function keyMapping(hostPlatform: string, appleClient: boolean): KeyMapping {
  const linuxHost = hostPlatform === "linux";
  const ctrlShortcut = linuxHost && !appleClient;
  return {
    commandAsCtrl: linuxHost && appleClient,
    shortcut: (event) => (ctrlShortcut ? event.ctrlKey : event.metaKey),
    tapKeys: linuxHost,
  };
}

export function modifiersOf(event: ModifierState, mapping?: KeyMapping): ScreenModifier[] {
  const result: ScreenModifier[] = [];
  const commandAsCtrl = mapping?.commandAsCtrl === true;
  if (event.metaKey && !commandAsCtrl) result.push("cmd");
  if (event.shiftKey) result.push("shift");
  if (event.altKey) result.push("alt");
  if (event.ctrlKey || (event.metaKey && commandAsCtrl)) result.push("ctrl");
  return result;
}
