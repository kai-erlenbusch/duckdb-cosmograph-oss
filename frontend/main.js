import { Graph } from '@cosmos.gl/graph';

const urlParams = new URLSearchParams(window.location.search);
const token = urlParams.get('token');

const canvas = document.getElementById('app');
const loading = document.getElementById('loading');

if (!token) {
    loading.innerText = 'Unauthorized: Missing session token. Please use the link printed in your DuckDB terminal (e.g. http://127.0.0.1:8080/?token=...).';
    throw new Error('Missing session token');
}

let graph = null;
let currentEdges = undefined;



async function initGraph() {
  try {
    const configRes = await fetch(`/config?token=${token}`).then(async r => {
        if (!r.ok) {
            const text = await r.text();
            throw new Error(text === "Unauthorized" ? "Invalid session token. Please ensure you are using the correct URL." : text);
        }
        return r.json();
    });
    const userConfig = configRes.config || {};
    
    // Edges are now fetched and parsed in the Web Worker for zero-copy transfer

    const worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });

    worker.onmessage = (e) => {
      if (e.data.error) {
        loading.innerText = 'Error loading graph: ' + e.data.error;
        return;
      }

      const { count, positions, sizes, colors, ids, edges } = e.data;
      loading.style.display = 'none';
      
      if (edges) {
          currentEdges = edges;
      }

      let useIds = ids && ids.some(val => val !== 0);

      if (graph) {
          try { graph.destroy(); } catch (e) { console.error("Error destroying graph:", e); }
      }

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

               if (!useIds) {
                   console.warn("No 'id' column found in query. Click details disabled.");
                   content.textContent = "Error: Click interactions on nodes require an explicit ID column in your query to prevent O(N) latency.";
                   return;
               }

               let queryParam = 'id_val=' + ids[index];

               fetch(`/node_details?${queryParam}&token=${token}`)
                 .then(async r => {
                   if (!r.ok) throw new Error(await r.text());
                   return r.json();
                 }).then(data => {
                   if (!data || data.length === 0) {
                       content.textContent = "Empty result! Node id:\n" + ids[index];
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

    worker.postMessage({ bounds: null, token: token });

  } catch(err) {
    loading.innerText = 'Error loading graph: ' + err.message;
    console.error(err);
  }
}

initGraph();
