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

    const xArr = new Float32Array(buf, 4, count);
    const yArr = new Float32Array(buf, 4 + count * 4, count);
    const rawSizeArr = new Float32Array(buf, 4 + count * 8, count);
    const rawColorArr = new Float32Array(buf, 4 + count * 12, count);

    let minSize = Infinity, maxSize = -Infinity;
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (let i = 0; i < count; i++) {
       if (rawSizeArr[i] < minSize) minSize = rawSizeArr[i];
       if (rawSizeArr[i] > maxSize) maxSize = rawSizeArr[i];
       if (xArr[i] < minX) minX = xArr[i];
       if (xArr[i] > maxX) maxX = xArr[i];
       if (yArr[i] < minY) minY = yArr[i];
       if (yArr[i] > maxY) maxY = yArr[i];
    }
    const logMin = Math.log1p(Math.max(0, minSize));
    const logMax = Math.log1p(Math.max(0, maxSize));
    
    const rangeX = maxX - minX || 1;
    const rangeY = maxY - minY || 1;
    const scale = Math.max(rangeX, rangeY);

    const positions = new Float32Array(count * 2);
    const sizes = new Float32Array(count);
    const colors = new Float32Array(count * 4);

    for (let i = 0; i < count; i++) {
      positions[i * 2] = xArr[i] * 40;
      positions[i * 2 + 1] = yArr[i] * 40;

      let t = logMax > logMin ? (Math.log1p(Math.max(0, rawSizeArr[i])) - logMin) / (logMax - logMin) : 0;
      sizes[i] = 1 + t * 2;

      const cat = Math.floor(rawColorArr[i]);
      const hex = getCategoryColor(cat);
      const [r, g, b, a] = hexToRGBA(hex);
      colors[i * 4] = r;
      colors[i * 4 + 1] = g;
      colors[i * 4 + 2] = b;
      colors[i * 4 + 3] = a;
    }
    
    self.postMessage({ count, positions, sizes, colors }, [positions.buffer, sizes.buffer, colors.buffer]);
  } catch(err) {
    self.postMessage({ error: err.message });
  }
};
