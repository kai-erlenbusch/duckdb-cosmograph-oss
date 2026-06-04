import { Graph } from '@cosmos.gl/graph';

const canvas = document.getElementById('app');
const loading = document.getElementById('loading');

let graph = null;
let currentEdges = undefined;



async function initGraph() {
  try {
    const configRes = await fetch('/config').then(r => r.json());
    const userConfig = configRes.config || {};
    
    const rawEdges = await fetch('/data/edges').then(r => r.json());
    if (rawEdges && rawEdges.source && rawEdges.target) {
        currentEdges = new Float32Array(rawEdges.source.length * 2);
        for (let i = 0; i < rawEdges.source.length; i++) {
            // NOTE: @cosmos.gl/graph expects zero-based INDICES into the points array!
            // Assuming for now that the edge source/target are already zero-based indices.
            currentEdges[i*2] = Number(rawEdges.source[i]);
            currentEdges[i*2+1] = Number(rawEdges.target[i]);
        }
    }

    const worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });

    worker.onmessage = (e) => {
      if (e.data.error) {
        loading.innerText = 'Error loading graph: ' + e.data.error;
        return;
      }

      const { count, positions, sizes, colors, ids } = e.data;
      loading.style.display = 'none';

      let useIds = ids && ids.some(val => val !== 0);

      graph = new Graph(canvas, {
        pixelRatio: 1, 
        renderHoveredPointRing: false,
        hoveredPointScale: 1.0,
        linkColor: 'white',
        fitViewOnInit: false,
        renderLinks: !!currentEdges,
        onClick: (index) => {
           if (index !== undefined && index !== -1) {
               const tooltip = document.getElementById('tooltip');
               const content = document.getElementById('tooltip-content');
               tooltip.style.display = 'block';
               content.innerText = 'Loading DuckDB data for node ' + index + '...';

               let queryParam = useIds ? 'id_val=' + ids[index] : 'id=' + index;
               if (!useIds) {
                   console.warn("No 'id' column found in query. Using OFFSET index which may be non-deterministic.");
               }

               fetch('/node_details?' + queryParam)
                 .then(async r => {
                   if (!r.ok) throw new Error(await r.text());
                   return r.json();
                 }).then(data => {
                   if (!data || data.length === 0) {
                       content.textContent = "Empty result! Node id:\n" + index;
                   } else {
                       let text = JSON.stringify(data[0] || data, null, 2);
                       if (text.length > 1000) text = text.substring(0, 1000) + '\n\n... [TRUNCATED]';
                       content.textContent = text;
                   }
                 }).catch(e => {
                   console.error(e);
                   content.textContent = "Error fetching node: " + e.message;
                 });
           }
        },
        ...userConfig,
        enableSimulation: false
      });
      
      graph.setPointPositions(positions);
      graph.setPointSizes(sizes);
      graph.setPointColors(colors);
      
      if (currentEdges) {
          graph.setLinks(currentEdges);
      }

      if (typeof graph.render === 'function') graph.render();

      setTimeout(() => {
          try {
              if (graph && typeof graph.setZoomTransformByPointPositions === 'function') {
                  graph.setZoomTransformByPointPositions(positions);
              }
          } catch(e) {
              console.error("Manual camera bounds fit failed:", e);
          }
      }, 500);

    };

    worker.postMessage({ bounds: null });

  } catch(err) {
    loading.innerText = 'Error loading graph: ' + err.message;
    console.error(err);
  }
}

initGraph();
