# DuckDB Cosmograph Extension

[![DuckDB](https://img.shields.io/badge/DuckDB-v1.0.0-orange.svg)](https://duckdb.org)
[![Cosmograph](https://img.shields.io/badge/Cosmograph-WebGL-blue.svg)](https://cosmograph.app/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A high-performance, serverless GPU graph explorer built as a native DuckDB extension.

This extension integrates the powerful GPU-accelerated [Cosmograph](https://cosmograph.app/) library directly into DuckDB. It allows you to visualize multi-million node graphs instantly from any DuckDB table or Parquet file using a simple SQL scalar function.

By running natively in DuckDB via C++ (instead of DuckDB-Wasm), this extension bypasses browser memory limits and leverages **Zero-Copy Binary Transport** to render massive datasets at 60fps.

## 🚀 Key Features

*   **Zero-Copy Web Worker Handoff:** DuckDB streams data using pure binary `Float32Array` and `Uint32Array` buffers. The frontend's Web Worker parses this binary stream and executes a zero-copy handoff directly to Cosmograph's WebGL context.
*   **Binary Data Transport:** Eliminates costly JSON serialization completely. Data is packed into highly-optimized 20-byte structs per node (`x, y, size, color, id`) in C++ and streamed as `application/octet-stream`.
*   **Serverless Architecture:** Spawns a lightweight, non-blocking HTTP server directly from SQL without needing Python, Node.js, or external infrastructure.
*   **Spatial Filter Pushdown:** The viewport's bounding box automatically pushes spatial filters (`WHERE x >= min_x AND ...`) down into DuckDB's optimized execution engine for interactive sub-graph queries.
*   **Real-time Callbacks:** Clicking any node in the UI triggers a real-time `SELECT` callback query in DuckDB, retrieving the node's full metadata instantly.

## 📦 Installation & Build

Ensure you have CMake and a C++ compiler (e.g., Visual Studio Build Tools on Windows).

```bash
# Clone the DuckDB extension template
git clone https://github.com/duckdb/extension-template duckdb-cosmograph
cd duckdb-cosmograph

# Build the extension in release mode
make release
```

## 🛠️ Quick Start

Load the extension and run the `serve_graph()` function to launch the explorer.

```sql
-- 1. Load the extension
LOAD 'build/release/extension/cosmograph/cosmograph.duckdb_extension';

-- 2. Create an in-memory table with required columns: id, x, y, size, color
--    NOTE: Creating an in-memory table is highly recommended to maximize performance.
--    CRITICAL: For massive datasets, you must CAST(id AS UINTEGER) and CREATE UNIQUE INDEX 
--    to ensure O(1) latency when clicking on nodes to fetch their metadata.
CREATE TABLE my_graph_nodes AS 
SELECT 
    (row_number() OVER())::UINTEGER AS id, 
    x::FLOAT AS x, 
    y::FLOAT AS y, 
    size_col::FLOAT AS size, 
    category_col::INTEGER AS color, 
    title AS label 
FROM 'my_dataset.parquet';

CREATE UNIQUE INDEX idx_graph_id ON my_graph_nodes(id);

-- 3. Launch the graph explorer with an optional JSON configuration!
SELECT serve_graph(
    'my_graph_nodes', 
    '{"hoveredPointScale": 3, "simulationGravity": 0.5, "spaceSize": 4096, "backgroundColor": "#222222", "simulationEnabled": false}'
);
```

Navigate to `http://localhost:8080/` in your browser to view the interactive graph.

To gracefully stop the server and return control to DuckDB, navigate to `http://localhost:8080/stop` or execute `SELECT stop_graph();` in your DuckDB client.

## 🏗️ Architecture

1.  **Backend (`cosmograph_extension.cpp`)**: A C++ DuckDB scalar function embeds a `cpp-httplib` web server on port 8080.
2.  **Data Pipeline**: When requested, DuckDB runs the SQL query, extracts the columns, and packs them into a continuous binary buffer via heavily optimized `std::memcpy` operations to ensure strict aliasing compliance.
3.  **Frontend**: The browser's Web Worker fetches the binary payload, scales it, maps color indices using an `O(1)` RGBA cache, and passes the typed arrays to Cosmograph's Data Kit for immediate GPU rendering.
4.  **Security**: The frontend is securely isolated using strict `Content-Security-Policy` headers, while web assets (HTML/JS) are embedded directly in the compiled C++ binary using `web_assets.hpp`.
