const fs = require('fs');
const path = require('path');

const distDir = path.join(__dirname, '../frontend/dist');
const headerPath = path.join(__dirname, '../src/include/web_assets.hpp');

if (!fs.existsSync(distDir)) {
    console.error('frontend/dist does not exist. Run npm run build in frontend first.');
    process.exit(1);
}

const files = [];
function walk(dir) {
    const items = fs.readdirSync(dir);
    for (const item of items) {
        const fullPath = path.join(dir, item);
        if (fs.statSync(fullPath).isDirectory()) {
            walk(fullPath);
        } else {
            files.push(fullPath);
        }
    }
}
walk(distDir);

let headerContent = `
#pragma once
#include <string>
#include <unordered_map>

namespace duckdb {
namespace web_assets {

struct Asset {
    const unsigned char* data;
    size_t size;
    const char* content_type;
};

`;

const assetEntries = [];
let assetCounter = 0;

for (const file of files) {
    const relPath = file.substring(distDir.length).replace(/\\/g, '/');
    const varName = `asset_${assetCounter++}`;
    const data = fs.readFileSync(file);
    
    // Create byte array string
    const hexArray = [];
    for (let i = 0; i < data.length; i++) {
        hexArray.push(`0x${data[i].toString(16).padStart(2, '0')}`);
    }
    
    headerContent += `static const unsigned char ${varName}[] = { ${hexArray.join(', ')} };\n`;
    
    let contentType = 'application/octet-stream';
    if (relPath.endsWith('.html')) contentType = 'text/html';
    else if (relPath.endsWith('.js')) contentType = 'application/javascript';
    else if (relPath.endsWith('.css')) contentType = 'text/css';
    else if (relPath.endsWith('.svg')) contentType = 'image/svg+xml';
    
    assetEntries.push(`    {"${relPath}", {${varName}, sizeof(${varName}), "${contentType}"}}`);
}

headerContent += `
static const std::unordered_map<std::string, Asset> get_assets() {
    return {
${assetEntries.join(',\n')}
    };
}

} // namespace web_assets
} // namespace duckdb
`;

fs.writeFileSync(headerPath, headerContent);
console.log('web_assets.hpp generated successfully.');
