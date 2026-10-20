import type { PluginClientContext, PluginSidebarItemProps } from "@getpaseo/plugin/client";
import { SidebarRow } from "@getpaseo/plugin/client/ui";
import { RemoteScreen } from "./client/screen";

function ScreenItem({ currentScreen, openScreen, host }: PluginSidebarItemProps) {
  // Every host with the plugin installed adds its own row, so name the host.
  return (
    <SidebarRow
      icon="Monitor"
      label={`Screen · ${host.label}`}
      active={currentScreen?.screenId === "screen"}
      onPress={() => openScreen({ screenId: "screen" })}
    />
  );
}

export default function contribute(client: PluginClientContext) {
  const cleanups = [
    client.addScreen({ id: "screen", title: "Host screen", Component: RemoteScreen }),
    client.addSidebarHeaderItem({ id: "screen", title: "Host screen", Component: ScreenItem }),
    client.addCommandCenterItem({
      id: "open-screen",
      title: "Show host screen",
      icon: "Monitor",
      keywords: ["remote", "screen", "desktop", "vnc", "control"],
      context: "global",
      onSelect: ({ openScreen }) => openScreen({ screenId: "screen" }),
    }),
  ];
  return () => {
    for (const cleanup of cleanups) cleanup();
  };
}
