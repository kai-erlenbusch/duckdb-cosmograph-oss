-- 1. Load the extension (change path if needed)
LOAD 'build/release/extension/cosmograph/cosmograph.duckdb_extension';

-- 2. Create an in-memory table to maximize performance for real-time interactions!
--    (Optional, but highly recommended for massive datasets to reduce latency)
CREATE TABLE my_graph_nodes AS 
SELECT 
    (row_number() OVER())::VARCHAR AS id, 
    x_umap AS x, 
    y_umap AS y, 
    num_of_tokens AS size, 
    model AS color, 
    content AS label 
FROM 'lmsys.parquet';

-- 3. Launch the graph explorer with an optional Cosmograph configuration!
SELECT serve_graph('my_graph_nodes', '{"simulationGravity": 0.5, "spaceSize": 4096, "backgroundColor": "#222222"}');
