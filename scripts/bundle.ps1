$distPath = 'D:\exploratory\duckdb-extension\duckdb-cosmograph\frontend\dist'
$files = Get-ChildItem -Path $distPath -File -Recurse

$output = 'D:\exploratory\duckdb-extension\duckdb-cosmograph\src\include\web_assets.hpp'

$cpp = @"
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace web_assets {

struct AssetInfo {
    const unsigned char* data;
    size_t size;
    const char* content_type;
};

inline std::unordered_map<std::string, AssetInfo> get_assets() {
    std::unordered_map<std::string, AssetInfo> assets;

"@

foreach ($file in $files) {
    $relativePath = $file.FullName.Substring($distPath.Length).Replace('\', '/')
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $varName = 'asset_' + [guid]::NewGuid().ToString('N')
    
    $contentType = 'text/plain'
    if ($file.Extension -eq '.html') { $contentType = 'text/html' }
    if ($file.Extension -eq '.js') { $contentType = 'application/javascript' }
    if ($file.Extension -eq '.css') { $contentType = 'text/css' }

    $cpp += "`n    static const unsigned char $varName[] = {`n"
    
    $hexBytes = @()
    foreach ($b in $bytes) {
        $hexBytes += '0x' + $b.ToString('X2')
    }
    $cpp += '        ' + ($hexBytes -join ', ')
    $cpp += "`n    };`n"
    $cpp += "    assets[`"$relativePath`"] = {$varName, $($bytes.Length), `"$contentType`"};`n"
}

$cpp += @"
    return assets;
}

} // namespace web_assets
} // namespace duckdb
"@

Set-Content -Path $output -Value $cpp
Write-Host 'Generated web_assets.hpp successfully'
