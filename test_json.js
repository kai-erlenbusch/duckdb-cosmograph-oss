fetch("http://localhost:8080/data/nodes?query=" + encodeURIComponent("SELECT x_umap, y_umap FROM 'lmsys.parquet' LIMIT 5"))
    .then(r => r.text())
    .then(console.log);
