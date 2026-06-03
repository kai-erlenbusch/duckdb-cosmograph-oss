# DuckDB Cosmograph Extension

A high-performance, serverless GPU graph explorer built as a native DuckDB extension.

This extension integrates the powerful GPU-accelerated [Cosmograph](https://cosmograph.app/) library directly into DuckDB. It allows you to visualize multi-million node graphs instantly from any DuckDB table or Parquet file using a simple SQL scalar function.

By running natively in DuckDB via C++ (instead of DuckDB-Wasm), this extension bypasses browser memory limits and leverages high-performance Columnar JSON streaming to render massive datasets at 60fps.

## Features

- **Serverless Architecture**: Spawns a lightweight, non-blocking HTTP server directly from SQL.
- **Massive Scale**: Streams data in a highly optimized columnar format, supporting massive real-world datasets (like the 340,000+ node HCP publications dataset) without crashing the browser.
- **Interactive Callbacks**: Clicking any node in the UI triggers a real-time `SELECT` callback query in DuckDB, retrieving the node's full metadata instantly.
- **Dynamic Configuration**: Pass custom JSON configurations from SQL to dynamically map colors, sizes, and physics simulations.

## Installation & Build

Ensure you have CMake and a C++ compiler (e.g., Visual Studio Build Tools on Windows).

```bash
# Clone the DuckDB extension template
git clone https://github.com/duckdb/extension-template duckdb-cosmograph
cd duckdb-cosmograph

# Build the extension in release mode
make release
```

## Usage

Load the extension and run the `serve_graph()` function to launch the explorer.

```sql
-- 1. Load the extension
LOAD 'build/release/extension/cosmograph/cosmograph.duckdb_extension';

-- 2. Create an in-memory table with required columns: id, x, y, size, color
--    NOTE: Creating an in-memory table is highly recommended to maximize performance 
--          for real-time node click queries and data streaming.
CREATE TABLE my_graph_nodes AS 
SELECT 
    (row_number() OVER())::VARCHAR AS id, 
    x, 
    y, 
    size_col AS size, 
    category_col AS color, 
    title AS label 
FROM 'my_dataset.parquet';

-- 3. Launch the graph explorer with an optional JSON configuration!
SELECT serve_graph(
    'my_graph_nodes', 
    '{"hoveredPointScale": 3, "simulationGravity": 0.5, "spaceSize": 4096, "backgroundColor": "#222222", "simulationEnabled": false}'
);
```

Navigate to `http://localhost:8080/` in your browser to view the interactive graph.

To gracefully stop the server and return control to DuckDB, navigate to `http://localhost:8080/stop` or execute `SELECT stop_graph();` in your DuckDB client.

## Architecture

1. **Backend (`cosmograph_extension.cpp`)**: A C++ DuckDB scalar function embeds a `cpp-httplib` web server on port 8080.
2. **Data Pipeline**: When requested, DuckDB runs the SQL query and serializes the result chunk into a highly compressed Columnar JSON payload.
3. **Frontend**: The browser fetches the payload, converts it into typed arrays (e.g., `Float32Array`), and passes it directly to Cosmograph's Data Kit for immediate GPU rendering.
4. **Callbacks**: Interaction events (`onClick`) trigger POST requests back to the DuckDB extension, resolving complex queries instantly.
