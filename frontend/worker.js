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
    const configRes = await fetch(self.location.origin + '/config');
    const configObj = await configRes.json();
    const config = configObj.config || {};
    const defaultScale = config.spatialScale || 1.0;

    let url = self.location.origin + '/data/nodes_binary';
    const bounds = e.data.bounds;
    if (bounds) {
      url += "?min_x=" + bounds.min_x + "&max_x=" + bounds.max_x + "&min_y=" + bounds.min_y + "&max_y=" + bounds.max_y;
    }
    const res = await fetch(url);
    const buf = await res.arrayBuffer();
    if (buf.byteLength === 0) {
      self.postMessage({ count: 0 });
      return;
    }

    const dv = new DataView(buf);
    const count = dv.getUint32(0, true);
    if (count === 0) {
      self.postMessage({ count: 0 });
      return;
    }

    const positions = new Float32Array(count * 2);
    const sizes = new Float32Array(count);
    const colors = new Float32Array(count * 4);
    const ids = new Uint32Array(count);

    let minSize = Infinity, maxSize = -Infinity;
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;

    // First pass: Calculate bounds for dynamic scaling
    for (let i = 0; i < count; i++) {
       const offset = 4 + (i * 20);
       const x = dv.getFloat32(offset, true);
       const y = dv.getFloat32(offset + 4, true);
       const s = dv.getFloat32(offset + 8, true);

       if (s < minSize) minSize = s;
       if (s > maxSize) maxSize = s;
       if (x < minX) minX = x;
       if (x > maxX) maxX = x;
       if (y < minY) minY = y;
       if (y > maxY) maxY = y;
    }

    const logMin = Math.log1p(Math.max(0, minSize));
    const logMax = Math.log1p(Math.max(0, maxSize));
    
    // Auto scale if they provided absolute coordinates
    const rangeX = maxX - minX || 1;
    const rangeY = maxY - minY || 1;
    const spatialRange = Math.max(rangeX, rangeY);
    // If the data is e.g. 0-1, we want to scale it to ~1000 so Cosmograph physics look good
    const autoScale = (spatialRange > 0 && spatialRange < 10) ? (1000.0 / spatialRange) : 1.0;
    const finalScale = autoScale * defaultScale;

    // Second pass: Populate arrays
    for (let i = 0; i < count; i++) {
      const offset = 4 + (i * 20);
      const x = dv.getFloat32(offset, true);
      const y = dv.getFloat32(offset + 4, true);
      const s = dv.getFloat32(offset + 8, true);
      const cat = Math.floor(dv.getFloat32(offset + 12, true));
      const id = dv.getUint32(offset + 16, true);

      positions[i * 2] = x * finalScale;
      positions[i * 2 + 1] = y * finalScale;

      let t = logMax > logMin ? (Math.log1p(Math.max(0, s)) - logMin) / (logMax - logMin) : 0;
      sizes[i] = config.minNodeSize || 1 + t * (config.maxNodeSize || 2);

      const hex = getCategoryColor(cat);
      const [r, g, b, a] = hexToRGBA(hex);
      colors[i * 4] = r;
      colors[i * 4 + 1] = g;
      colors[i * 4 + 2] = b;
      colors[i * 4 + 3] = a;

      ids[i] = id;
    }
    
    self.postMessage({ count, positions, sizes, colors, ids }, [positions.buffer, sizes.buffer, colors.buffer, ids.buffer]);
  } catch(err) {
    self.postMessage({ error: err.message });
  }
};
