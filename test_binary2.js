fetch('http://localhost:8080/data/nodes_binary').then(r => r.arrayBuffer()).then(b => {
    const v = new Uint8Array(b);
    console.log('Length:', v.length);
    const count = new Uint32Array(v.buffer, 0, 1)[0];
    console.log('Count:', count);
    
    // Dump 10 nodes evenly spaced
    for(let k=0; k<10; k++) {
        let i = Math.floor((count - 1) * (k/9));
        let offset = 4 + (i * 20);
        const chunk = v.slice(offset, offset+20);
        const x = new Float32Array(chunk.buffer)[0];
        const y = new Float32Array(chunk.buffer)[1];
        const s = new Float32Array(chunk.buffer)[2];
        const c = new Float32Array(chunk.buffer)[3];
        const id = new Uint32Array(chunk.buffer)[4];
        console.log(`Node ${i} (offset ${offset}): x=${x} y=${y} s=${s} c=${c} id=${id}`);
    }
});
