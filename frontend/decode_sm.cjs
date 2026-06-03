const fs = require('fs');
const { SourceMapConsumer } = require('source-map');

const rawSourceMap = JSON.parse(fs.readFileSync('dist/assets/index.js.map', 'utf8'));

SourceMapConsumer.with(rawSourceMap, null, consumer => {
  console.log(consumer.originalPositionFor({
    line: 317,
    column: 1116
  }));
  
  console.log(consumer.originalPositionFor({
    line: 317,
    column: 1048
  }));
});
