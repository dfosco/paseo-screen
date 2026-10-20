import { memo, useCallback, useEffect, useState } from "react";
import { Image, View } from "react-native";

interface Layers {
  uris: [string | null, string | null];
  /** Bumped whenever a layer gets a new frame; part of the layer's key so it remounts. */
  versions: [number, number];
  front: 0 | 1;
}

/**
 * Shows a stream of frames without flashing. Changing an Image's source blanks it until the new
 * image decodes (very visible on iOS), so each new frame loads in a hidden layer that becomes the
 * visible one only after it has loaded.
 *
 * Memoized, and every layer keeps a stable onLoad: react-native-web restarts an image load whenever
 * onLoad changes, so re-rendering on each stats update would abort every load before it finished.
 */
export const FrameImage = memo(function FrameImage({ uri, width, height }: { uri: string; width: number; height: number }) {
  const [layers, setLayers] = useState<Layers>({ uris: [uri, null], versions: [0, 0], front: 0 });

  useEffect(() => {
    setLayers((previous) => {
      if (previous.uris.includes(uri)) return previous;
      const back = previous.front === 0 ? 1 : 0;
      const uris: Layers["uris"] = [...previous.uris];
      const versions: Layers["versions"] = [...previous.versions];
      // A frame still loading in the back layer is replaced; only the newest one matters.
      uris[back] = uri;
      versions[back] += 1;
      return { uris, versions, front: previous.front };
    });
  }, [uri]);

  const onLayerLoad = useCallback((index: 0 | 1, layerUri: string) => {
    setLayers((previous) =>
      previous.front !== index && previous.uris[index] === layerUri ? { ...previous, front: index } : previous,
    );
  }, []);

  return (
    <View style={{ width, height }}>
      {layers.uris.map((layerUri, index) =>
        layerUri ? (
          <FrameLayer
            // A new key per source means a late onLoad from a replaced frame can never flip layers.
            key={`${index}:${layers.versions[index]}`}
            index={index as 0 | 1}
            uri={layerUri}
            visible={index === layers.front}
            width={width}
            height={height}
            onLayerLoad={onLayerLoad}
          />
        ) : null,
      )}
    </View>
  );
});

const FrameLayer = memo(function FrameLayer({ index, uri, visible, width, height, onLayerLoad }: {
  index: 0 | 1;
  uri: string;
  visible: boolean;
  width: number;
  height: number;
  onLayerLoad(index: 0 | 1, uri: string): void;
}) {
  const onLoad = useCallback(() => onLayerLoad(index, uri), [onLayerLoad, index, uri]);
  // A stable source object, so the image is not treated as a new source on re-render.
  const [source] = useState(() => ({ uri }));
  return (
    <Image
      source={source}
      style={{ position: "absolute", left: 0, top: 0, width, height, opacity: visible ? 1 : 0, zIndex: visible ? 1 : 0 }}
      resizeMode="stretch"
      fadeDuration={0}
      onLoad={onLoad}
    />
  );
});
