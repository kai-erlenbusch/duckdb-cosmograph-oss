fetch('http://localhost:8080/data/nodes_binary').then(r => r.arrayBuffer()).then(b => {
    const v = new Uint8Array(b);
    console.log('Length:', v.length);
    for(let i=0; i<44; i+=4) {
        const chunk = v.slice(i, i+4);
        const f = new Float32Array(chunk.buffer)[0];
        const u = new Uint32Array(chunk.buffer)[0];
        console.log(`Offset ${i}: hex=${Buffer.from(chunk).toString('hex')} float=${f} uint=${u}`);
    }
});
