import type { PluginTheme } from "@getpaseo/plugin";
import type { PluginScreenProps } from "@getpaseo/plugin/client";
import { useRpc } from "@getpaseo/plugin/client";
import { useQuery, useMutation } from "@tanstack/react-query";
import { useCallback, useEffect, useMemo, useState } from "react";
import { Platform, Pressable, ScrollView, Text, TextInput, View, type LayoutChangeEvent } from "react-native";
import { chooseDisplayRpc, requestPermissionsRpc, statusRpc, type ScreenDisplay, type ScreenInputEvent, type ScreenModifier, type ScreenStatus } from "../shared/rpc";
import { readLocalClipboard, useHostClipboard } from "./clipboard";
import { FrameImage } from "./frame-image";
import { fitContent } from "./geometry";
import { KEY } from "./keys";
import { TouchStage } from "./touch-stage";
import { QUALITIES, useFrames, useInputQueue } from "./use-screen";
import { useWebInput } from "./web-input";

const isWeb = Platform.OS === "web";

export function RemoteScreen({ theme, layout, host }: PluginScreenProps) {
  const styles = useMemo(() => createStyles(theme), [theme]);
  const fetchStatus = useRpc(statusRpc);
  const status = useQuery({ queryKey: ["paseo-screen", "status"], queryFn: () => fetchStatus({}), refetchInterval: 5000 });
  const requestPermissions = useRpc(requestPermissionsRpc);
  const request = useMutation({ mutationFn: () => requestPermissions({}), onSuccess: () => void status.refetch() });

  const [paused, setPaused] = useState(false);
  const [control, setControl] = useState(true);
  const [qualityIndex, setQualityIndex] = useState(1);
  const [chosenDisplay, setChosenDisplay] = useState<number | null>(null);
  const chooseDisplayOnHost = useRpc(chooseDisplayRpc);
  const [inputError, setInputError] = useState<string | null>(null);

  const displays = status.data?.displays ?? [];
  // This viewer's choice, else the display last chosen on this host, else the main display.
  const display =
    displays.find((candidate) => candidate.id === chosenDisplay) ??
    displays.find((candidate) => candidate.id === status.data?.preferredDisplay) ??
    displays.find((candidate) => candidate.main) ??
    displays[0];
  const displayId = display?.id;
  const chooseDisplay = useCallback(
    (id: number) => {
      setChosenDisplay(id);
      void chooseDisplayOnHost({ display: id }).catch(() => undefined);
    },
    [chooseDisplayOnHost],
  );
  const nextDisplay = () => {
    if (displays.length < 2) return;
    const index = displays.findIndex((candidate) => candidate.id === displayId);
    chooseDisplay(displays[(index + 1) % displays.length]!.id);
  };
  const canCapture = status.data?.screenRecording === true;
  const [stage, setStage] = useState({ width: 0, height: 0 });
  const [rotated, setRotated] = useState(false);
  const [zoom, setZoom] = useState(1);
  const [aspect, setAspect] = useState(16 / 10);
  // The touch viewer can turn the picture; the web viewer always shows it upright.
  const content = fitContent(stage, aspect, !isWeb && rotated);
  // Zoom in whole steps for resolution, so pinching does not restart the frame loop continuously.
  const frames = useFrames(canCapture && !paused, displayId, QUALITIES[qualityIndex]!, content.width * Math.ceil(zoom));
  const frameAspect = frames.display ? frames.display.width / frames.display.height : null;
  useEffect(() => {
    if (frameAspect) setAspect(frameAspect);
  }, [frameAspect]);
  // After a drag carries the pointer onto another display, show that display.
  const send = useInputQueue(displayId, setInputError, chooseDisplay);
  const controlling = control && status.data?.accessibility === true;

  const clipboard = useHostClipboard(controlling);
  const pasteOnHost = useCallback(
    (text: string) => {
      send({ type: "paste", text });
      clipboard.notePasted(text);
    },
    [send, clipboard.notePasted],
  );
  const pasteLocalClipboard = useCallback(async () => {
    const text = await readLocalClipboard();
    if (text) pasteOnHost(text);
    else clipboard.setMessage("Could not read this device's clipboard");
  }, [pasteOnHost, clipboard.setMessage]);

  const [viewNode, setViewNode] = useState<unknown>(null);
  useWebInput(viewNode, isWeb && controlling, send, {
    onHostCopy: clipboard.afterHostCopy,
    onPaste: pasteOnHost,
    onPasteFallback: () => void pasteLocalClipboard(),
  });


  return (
    <View style={styles.screen}>
      <View style={styles.toolbar}>
        <Text style={styles.hostLabel} numberOfLines={1}>{host.label}</Text>
        <ToolButton styles={styles} label={paused ? "Resume" : "Pause"} onPress={() => setPaused(!paused)} />
        <ToolButton styles={styles} label={control ? "Control: on" : "View only"} active={control} onPress={() => setControl(!control)} />
        <ToolButton styles={styles} label={QUALITIES[qualityIndex]!.label} onPress={() => setQualityIndex((qualityIndex + 1) % QUALITIES.length)} />
        {!isWeb ? <ToolButton styles={styles} label="Rotate" active={rotated} onPress={() => setRotated(!rotated)} /> : null}
        {displays.length > 1 ? (
          <ToolButton
            styles={styles}
            label={`🖥 ${display ? displayLabel(display) : "Display"} (${displays.findIndex((candidate) => candidate.id === displayId) + 1}/${displays.length})`}
            onPress={nextDisplay}
          />
        ) : null}
        <Text style={styles.meta}>
          {frames.display ? `${frames.fps.toFixed(1)} fps · ${frames.latencyMs} ms · ${frames.kilobytes} KB` : ""}
          {clipboard.message ? ` · ${clipboard.message}` : ""}
        </Text>
      </View>

      <PermissionBanner styles={styles} status={status.data} loading={status.isLoading} requesting={request.isPending} onRequest={() => request.mutate()} />
      {frames.error || inputError ? <Text style={styles.error}>{frames.error ?? inputError}</Text> : null}
      {frames.locked ? (
        <View style={styles.banner}>
          <Text style={styles.bannerText}>
            {isWeb
              ? "This Mac is locked. Click the picture, type your password and press Return to unlock it."
              : "This Mac is locked. Type your password in the box below and press ⏎ to unlock it."}
          </Text>
        </View>
      ) : null}

      <View style={styles.stage} onLayout={(event: LayoutChangeEvent) => setStage(event.nativeEvent.layout)}>
        {frames.uri ? (
          isWeb ? (
            <View ref={setViewNode} accessibilityLabel="Remote screen" style={{ width: content.width, height: content.height }}>
              <FrameImage uri={frames.uri} width={content.width} height={content.height} />
            </View>
          ) : (
            <TouchStage
              uri={frames.uri}
              stage={stage}
              content={content}
              rotated={rotated}
              controlling={controlling}
              send={send}
              onZoomChange={setZoom}
            />
          )
        ) : (
          <Text style={styles.placeholder}>{canCapture ? (paused ? "Paused" : "Waiting for the first frame…") : "No screen yet"}</Text>
        )}
      </View>

      {controlling ? (
        <KeyBar
          styles={styles}
          compact={layout.compact}
          send={send}
          clipboard={clipboard.supported ? { copyFromHost: clipboard.pull, pasteToHost: () => void pasteLocalClipboard() } : null}
        />
      ) : null}
    </View>
  );
}

function PermissionBanner({ styles, status, loading, requesting, onRequest }: {
  styles: Styles;
  status: ScreenStatus | undefined;
  loading: boolean;
  requesting: boolean;
  onRequest(): void;
}) {
  if (loading || !status) return null;
  if (!status.supported || status.error) return <Text style={styles.error}>{status.error ?? "Unsupported host."}</Text>;
  const missing = [!status.screenRecording && "Screen Recording", !status.accessibility && "Accessibility"].filter(Boolean);
  if (missing.length === 0) return null;
  return (
    <View style={styles.banner}>
      <Text style={styles.bannerText}>
        This Mac has not given Paseo {missing.join(" and ")} permission. Someone at the host Mac has to allow it once in System Settings, Privacy and Security, then restart Paseo there.
      </Text>
      <ToolButton styles={styles} label={requesting ? "Asking…" : "Show prompt on host"} onPress={onRequest} />
    </View>
  );
}

function KeyBar({ styles, compact, send, clipboard }: {
  styles: Styles;
  compact: boolean;
  send(event: ScreenInputEvent): void;
  clipboard: { copyFromHost(): void; pasteToHost(): void } | null;
}) {
  const [text, setText] = useState("");
  const key = (keyCode: number, modifiers: ScreenModifier[] = []) => () => send({ type: "key", keyCode, modifiers });
  const submit = (withEnter: boolean) => {
    // Text events carry at most 10,000 UTF-16 units each; split on code points so no emoji is cut in half.
    let chunk = "";
    for (const codePoint of text) {
      if (chunk.length + codePoint.length > 10_000) {
        send({ type: "text", text: chunk });
        chunk = "";
      }
      chunk += codePoint;
    }
    if (chunk) send({ type: "text", text: chunk });
    if (withEnter) send({ type: "key", keyCode: KEY.enter });
    setText("");
  };
  return (
    <ScrollView horizontal style={styles.keyBarScroll} contentContainerStyle={styles.keyBar} keyboardShouldPersistTaps="handled">
      <TextInput
        style={styles.textInput}
        value={text}
        onChangeText={setText}
        placeholder="Type on the host…"
        placeholderTextColor={styles.meta.color}
        onSubmitEditing={() => submit(true)}
        autoCapitalize="none"
        autoCorrect={false}
      />
      <ToolButton styles={styles} label="Send" onPress={() => submit(false)} />
      <ToolButton styles={styles} label="Esc" onPress={key(KEY.escape)} />
      <ToolButton styles={styles} label="Tab" onPress={key(KEY.tab)} />
      <ToolButton styles={styles} label="⏎" onPress={key(KEY.enter)} />
      <ToolButton styles={styles} label="⌫" onPress={key(KEY.backspace)} />
      <ToolButton styles={styles} label="⌘Tab" onPress={key(KEY.tab, ["cmd"])} />
      <ToolButton styles={styles} label="⌘Space" onPress={key(KEY.space, ["cmd"])} />
      <ToolButton styles={styles} label="⌘W" onPress={() => send({ type: "key", char: "w", modifiers: ["cmd"] })} />
      <ToolButton styles={styles} label="⌘Q" onPress={() => send({ type: "key", char: "q", modifiers: ["cmd"] })} />
      <ToolButton styles={styles} label="◀ Space" onPress={key(KEY.left, ["ctrl"])} />
      <ToolButton styles={styles} label="Space ▶" onPress={key(KEY.right, ["ctrl"])} />
      <ToolButton styles={styles} label="Mission Control" onPress={key(KEY.up, ["ctrl"])} />
      <ToolButton styles={styles} label="App windows" onPress={key(KEY.down, ["ctrl"])} />
      {clipboard ? (
        <>
          <ToolButton styles={styles} label="Copy from host" onPress={clipboard.copyFromHost} />
          <ToolButton styles={styles} label="Paste to host" onPress={clipboard.pasteToHost} />
        </>
      ) : null}
      {compact ? (
        <>
          <ToolButton styles={styles} label="←" onPress={key(KEY.left)} />
          <ToolButton styles={styles} label="↑" onPress={key(KEY.up)} />
          <ToolButton styles={styles} label="↓" onPress={key(KEY.down)} />
          <ToolButton styles={styles} label="→" onPress={key(KEY.right)} />
          <ToolButton styles={styles} label="Scroll ↑" onPress={() => send({ type: "scroll", dx: 0, dy: -300 })} />
          <ToolButton styles={styles} label="Scroll ↓" onPress={() => send({ type: "scroll", dx: 0, dy: 300 })} />
        </>
      ) : null}
    </ScrollView>
  );
}

function ToolButton({ styles, label, active, onPress }: { styles: Styles; label: string; active?: boolean; onPress(): void }) {
  return (
    <Pressable accessibilityRole="button" accessibilityLabel={label} style={[styles.button, active ? styles.buttonActive : null]} onPress={onPress}>
      <Text style={[styles.buttonText, active ? styles.buttonTextActive : null]}>{label}</Text>
    </Pressable>
  );
}

function displayLabel(display: ScreenDisplay): string {
  const name = display.builtin ? "Built-in" : display.name;
  return display.main ? `${name}, main` : name;
}

type Styles = ReturnType<typeof createStyles>;

function createStyles(theme: PluginTheme) {
  const { colors } = theme;
  return {
    screen: { flex: 1, backgroundColor: colors.surface0 },
    toolbar: { flexDirection: "row" as const, flexWrap: "wrap" as const, alignItems: "center" as const, gap: 6, padding: 8, borderBottomWidth: 1, borderColor: colors.border },
    hostLabel: { color: colors.foreground, fontSize: 13, fontWeight: "600" as const, marginRight: 4, maxWidth: 220 },
    meta: { color: colors.foregroundMuted, fontSize: 12, marginLeft: 4 },
    button: { paddingHorizontal: 10, paddingVertical: 6, borderRadius: 6, backgroundColor: colors.surface2 },
    buttonActive: { backgroundColor: colors.accent },
    buttonText: { color: colors.foreground, fontSize: 13 },
    buttonTextActive: { color: colors.accentForeground },
    banner: { flexDirection: "row" as const, flexWrap: "wrap" as const, alignItems: "center" as const, gap: 8, padding: 10, backgroundColor: colors.surface1, borderBottomWidth: 1, borderColor: colors.statusWarning },
    bannerText: { flex: 1, minWidth: 240, color: colors.foreground, fontSize: 13 },
    error: { color: colors.statusDanger, fontSize: 12, paddingHorizontal: 10, paddingVertical: 4 },
    stage: { flex: 1, alignItems: "center" as const, justifyContent: "center" as const, backgroundColor: "#000" },
    placeholder: { color: colors.foregroundMuted },
    keyBarScroll: { flexGrow: 0, borderTopWidth: 1, borderColor: colors.border },
    keyBar: { flexDirection: "row" as const, alignItems: "center" as const, gap: 6, padding: 8 },
    textInput: { minWidth: 180, paddingHorizontal: 10, paddingVertical: 6, borderRadius: 6, borderWidth: 1, borderColor: colors.border, color: colors.foreground, backgroundColor: colors.surface1 },
  };
}
