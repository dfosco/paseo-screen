import { useRpc } from "@getpaseo/plugin/client";
import { useCallback, useEffect, useRef, useState } from "react";
import { Platform } from "react-native";
import { clipboardRpc } from "../shared/rpc";

// This plugin typechecks without the DOM library. Declare only what this module uses.
declare const navigator: { clipboard?: { writeText(text: string): Promise<void>; readText(): Promise<string> } };

const hasWebClipboard = () => Platform.OS === "web" && typeof navigator !== "undefined" && navigator.clipboard !== undefined;

export async function readLocalClipboard(): Promise<string | null> {
  if (!hasWebClipboard()) return null;
  try {
    return await navigator.clipboard!.readText();
  } catch {
    return null;
  }
}

/**
 * Mirrors host copies into this client's clipboard (web and desktop clients only).
 * After a copy shortcut is sent, the host clipboard is checked a few times because apps copy asynchronously.
 */
export function useHostClipboard(active: boolean) {
  const read = useRpc(clipboardRpc);
  const readRef = useRef(read);
  readRef.current = read;
  const known = useRef<number | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const supported = hasWebClipboard();

  useEffect(() => {
    if (!active || !supported) return;
    readRef.current({}).then(
      (clipboard) => (known.current = clipboard.changeCount),
      () => undefined,
    );
  }, [active, supported]);

  useEffect(() => {
    if (!message) return;
    const timer = setTimeout(() => setMessage(null), 4000);
    return () => clearTimeout(timer);
  }, [message]);

  /** Copies the host clipboard here. With `onlyIfChanged`, skips it unless something on the host copied since the last check. */
  const pull = useCallback(
    async (onlyIfChanged: boolean): Promise<boolean> => {
      if (!supported) return false;
      try {
        const clipboard = await readRef.current({});
        if (onlyIfChanged && clipboard.changeCount === known.current) return false;
        known.current = clipboard.changeCount;
        if (clipboard.text === undefined) {
          setMessage("Host clipboard has no text");
          return true;
        }
        await navigator.clipboard!.writeText(clipboard.text);
        setMessage(`Copied ${clipboard.text.length} characters from host`);
        return true;
      } catch (error) {
        setMessage(`Clipboard sync failed: ${error instanceof Error ? error.message : String(error)}`);
        return true;
      }
    },
    [supported],
  );

  const afterHostCopy = useCallback(() => {
    let attempt = 0;
    const delays = [150, 350, 700, 1200];
    const check = () => {
      void pull(true).then((done) => {
        if (!done && ++attempt < delays.length) setTimeout(check, delays[attempt]! - delays[attempt - 1]!);
      });
    };
    setTimeout(check, delays[0]);
  }, [pull]);

  /** Records that the host clipboard was just set from here, so it is not mirrored back. */
  const notePasted = useCallback(() => {
    void readRef.current({}).then(
      (clipboard) => (known.current = clipboard.changeCount),
      () => undefined,
    );
  }, []);

  return { supported, message, pull: () => void pull(false), afterHostCopy, notePasted, setMessage };
}
