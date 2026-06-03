const http = require('http');
http.get('http://127.0.0.1:8080/', (res) => {
  let data = '';
  res.on('data', (c) => data += c);
  res.on('end', () => {
    const jsFile = data.split('src="/assets/')[1].split('"')[0];
    http.get('http://127.0.0.1:8080/assets/' + jsFile, (res2) => {
      let data2 = '';
      res2.on('data', (c) => data2 += c);
      res2.on('end', () => {
        console.log('FORCE OVERRIDE?', data2.includes('FORCE OVERRIDE'));
        console.log('x40?', data2.includes('* 40') || data2.includes('*40') || data2.includes('x40'));
        console.log('Worker URL check (is worker.js built?):');
        // Let's also check if we can fetch the worker
        const workerMatch = data2.match(/new URL\("([^"]+)"/);
        console.log('Worker match:', workerMatch ? workerMatch[1] : 'none');
      });
    });
  });
});
