@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\exploratory\duckdb-extension\duckdb-cosmograph-oss
cd frontend
call npm install
call npm run build
cd ..
node scripts/embed_assets.js
cmake -B build -S .
cmake --build build --config Release
