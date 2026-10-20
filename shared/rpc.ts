import { defineRpc } from "@getpaseo/plugin";
import { z } from "zod";

export const displaySchema = z.object({
  id: z.number(),
  x: z.number(),
  y: z.number(),
  /** Size in points; input coordinates map onto this rectangle. */
  width: z.number(),
  height: z.number(),
  pixelWidth: z.number(),
  pixelHeight: z.number(),
  main: z.boolean(),
  builtin: z.boolean(),
  /** Monitor name as shown in System Settings, such as "Built-in Retina Display". */
  name: z.string(),
});
export type ScreenDisplay = z.infer<typeof displaySchema>;

const statusSchema = z.object({
  supported: z.boolean(),
  /** Set when the helper could not be built or started. */
  error: z.string().nullable(),
  screenRecording: z.boolean(),
  accessibility: z.boolean(),
  /** The host is showing its lock screen. */
  locked: z.boolean(),
  displays: z.array(displaySchema),
  /** The display a viewer last chose on this host, if it is still connected. */
  preferredDisplay: z.number().nullable(),
});
export type ScreenStatus = z.infer<typeof statusSchema>;

export const statusRpc = defineRpc({
  name: "screen.status",
  input: z.object({}),
  output: statusSchema,
});

/** Shows the macOS permission prompts on the host and opens System Settings when a prompt is no longer offered. */
export const requestPermissionsRpc = defineRpc({
  name: "screen.request-permissions",
  input: z.object({}),
  output: statusSchema,
});

export const frameRpc = defineRpc({
  name: "screen.frame",
  input: z.object({
    display: z.number().optional(),
    maxWidth: z.number().int().min(320).max(8192),
    quality: z.number().min(0.1).max(1),
    /** Hash of the frame the client already shows; an identical frame comes back without image data. */
    since: z.string().optional(),
  }),
  output: z.object({
    display: displaySchema,
    imageWidth: z.number(),
    imageHeight: z.number(),
    hash: z.string(),
    bytes: z.number(),
    /** The host is showing its lock screen. */
    locked: z.boolean(),
    unchanged: z.boolean().optional(),
    jpeg: z.string().optional(),
  }),
});
export type ScreenFrame = z.infer<typeof frameRpc.output>;

const point = { x: z.number().min(0).max(1), y: z.number().min(0).max(1) };
/** Pointer positions during a drag may leave the viewed display (below 0 or above 1) to continue onto a neighbouring one. */
const dragPoint = { x: z.number().min(-4).max(5), y: z.number().min(-4).max(5) };
const button = z.enum(["left", "right", "middle"]);
const modifier = z.enum(["cmd", "shift", "alt", "ctrl", "fn"]);
const modifiers = z.array(modifier).optional();

export const inputEventSchema = z.discriminatedUnion("type", [
  z.object({ type: z.literal("move"), ...dragPoint, modifiers }),
  z.object({ type: z.literal("down"), ...point, button, clickCount: z.number().int().min(1).optional(), modifiers }),
  z.object({ type: z.literal("up"), ...dragPoint, button, clickCount: z.number().int().min(1).optional(), modifiers }),
  z.object({ type: z.literal("click"), ...point, button, count: z.number().int().min(1).max(3).optional(), modifiers }),
  z.object({ type: z.literal("scroll"), x: point.x.optional(), y: point.y.optional(), dx: z.number().min(-10_000).max(10_000), dy: z.number().min(-10_000).max(10_000) }),
  /**
   * A macOS virtual key code (a physical key position), or `char`, a character the host maps to a key
   * on its own keyboard layout. Omit `down` to press and release.
   */
  z
    .object({
      type: z.literal("key"),
      keyCode: z.number().int().min(0).max(127).optional(),
      char: z.string().min(1).max(2).optional(),
      down: z.boolean().optional(),
      modifiers,
    })
    .refine((event) => event.keyCode !== undefined || event.char !== undefined, "key needs keyCode or char"),
  z.object({ type: z.literal("text"), text: z.string().max(10_000) }),
  /** Sets the host clipboard to `text`, then presses Cmd+V on the host. */
  z.object({ type: z.literal("paste"), text: z.string().max(1_000_000) }),
]);
export type ScreenInputEvent = z.infer<typeof inputEventSchema>;
export type ScreenModifier = z.infer<typeof modifier>;

export const inputRpc = defineRpc({
  name: "screen.input",
  input: z.object({ display: z.number().optional(), events: z.array(inputEventSchema).max(200) }),
  output: z.object({
    handled: z.number(),
    /** Display the pointer ended on, which differs from `display` after a drag onto another display. */
    pointerDisplay: z.number().optional(),
  }),
});

/** The host's plain-text clipboard. `changeCount` increases whenever anything on the host copies. */
export const clipboardRpc = defineRpc({
  name: "screen.clipboard",
  input: z.object({}),
  output: z.object({ changeCount: z.number(), text: z.string().optional() }),
});

/** Remembers the display a viewer chose, so the next viewer opens on it. */
export const chooseDisplayRpc = defineRpc({
  name: "screen.choose-display",
  input: z.object({ display: z.number() }),
  output: z.object({}),
});
