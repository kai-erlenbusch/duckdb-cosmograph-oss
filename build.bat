call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\exploratory\duckdb-extension\duckdb-cosmograph-oss\frontend
call npx vite build
cd ..
node scripts/embed_assets.js
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --config Release
