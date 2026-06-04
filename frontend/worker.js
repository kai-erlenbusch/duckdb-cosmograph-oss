import { } from 'apache-arrow'; // Removed

const palette = ['#e6194B', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#911eb4', '#42d4f4', '#f032e6', '#bfef45', '#fabed4', '#469990', '#dcbeff', '#9A6324', '#fffac8', '#800000', '#aaffc3', '#808000', '#ffd8b1', '#000075'];
const colorMap = new Map();

function getCategoryColor(cat) {
  if (!cat) return '#666666';
  if (!colorMap.has(cat)) colorMap.set(cat, palette[colorMap.size % palette.length]);
  return colorMap.get(cat);
}

function hexToRGBA(hex) {
    const r = parseInt(hex.slice(1, 3), 16) / 255;
    const g = parseInt(hex.slice(3, 5), 16) / 255;
    const b = parseInt(hex.slice(5, 7), 16) / 255;
    return [r, g, b, 1.0];
}

self.onmessage = async (e) => {
  try {
    const token = e.data.token;
    const configRes = await fetch(self.location.origin + `/config?token=${token}`);
    const configObj = await configRes.json();
    const config = configObj.config || {};
    const defaultScale = config.spatialScale || 1.0;

    let url = self.location.origin + `/data/nodes_binary?token=${token}`;
    const bounds = e.data.bounds;
    if (bounds) {
      url += "&min_x=" + bounds.min_x + "&max_x=" + bounds.max_x + "&min_y=" + bounds.min_y + "&max_y=" + bounds.max_y;
    }
    const res = await fetch(url);
    const buf = await res.arrayBuffer();
    if (buf.byteLength === 0) {
      self.postMessage({ count: 0 });
      return;
    }

    const count = Math.floor(buf.byteLength / 20);
    if (count === 0) {
      self.postMessage({ count: 0 });
      return;
    }

    const floatView = new Float32Array(buf);
    const uintView = new Uint32Array(buf);

    const positions = new Float32Array(count * 2);
    const sizes = new Float32Array(count);
    const colors = new Float32Array(count * 4);
    const ids = new Uint32Array(count);

    // Simple fixed scaling to prevent points clamping to the bounds (The Fix from NotebookLM)
    const PHYSICS_LAYOUT_SCALE = 40.0;
    const finalScale = PHYSICS_LAYOUT_SCALE * defaultScale;

    let minSize = Infinity, maxSize = -Infinity;
    const rgbaCache = [];

    // First pass: Calculate bounds for dynamic node sizing only
    for (let i = 0; i < count; i++) {
       const s = floatView[i * 5 + 2];

       if (s < minSize) minSize = s;
       if (s > maxSize) maxSize = s;
    }

    const logMin = Math.log1p(Math.max(0, minSize));
    const logMax = Math.log1p(Math.max(0, maxSize));    // Second pass: Populate arrays
    for (let i = 0; i < count; i++) {
      const x = floatView[i * 5];
      const y = floatView[i * 5 + 1];
      const s = floatView[i * 5 + 2];
      const cat = Math.floor(floatView[i * 5 + 3]);
      const id = uintView[i * 5 + 4];

      positions[i * 2] = x * finalScale;
      positions[i * 2 + 1] = y * finalScale;

      let t = logMax > logMin ? (Math.log1p(Math.max(0, s)) - logMin) / (logMax - logMin) : 0;
      sizes[i] = config.minNodeSize || 1 + t * (config.maxNodeSize || 2);

      let rgba = rgbaCache[cat];
      if (!rgba) {
          const hex = getCategoryColor(cat);
          rgba = hexToRGBA(hex);
          rgbaCache[cat] = rgba;
      }
      colors[i * 4] = rgba[0];
      colors[i * 4 + 1] = rgba[1];
      colors[i * 4 + 2] = rgba[2];
      colors[i * 4 + 3] = rgba[3];

      ids[i] = id;
    }
    
    let edges = null;
    try {
        const edgeRes = await fetch(self.location.origin + `/data/edges_binary?token=${token}`);
        if (edgeRes.ok) {
            const edgeBuf = await edgeRes.arrayBuffer();
            if (edgeBuf.byteLength >= 8) {
                const edgeCount = Math.floor(edgeBuf.byteLength / 8);
                if (edgeCount > 0) {
                    edges = new Float32Array(edgeBuf);
                }
            }
        }
    } catch(err) {
        console.error("Error fetching edges:", err);
    }
    
    const transferables = [positions.buffer, sizes.buffer, colors.buffer, ids.buffer];
    if (edges) {
        transferables.push(edges.buffer);
    }
    self.postMessage({ count, positions, sizes, colors, ids, edges }, transferables);
  } catch(err) {
    self.postMessage({ error: err.message });
  }
};
