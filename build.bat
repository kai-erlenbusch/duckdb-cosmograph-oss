@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd D:\exploratory\duckdb-extension\duckdb-cosmograph-oss
cd frontend
call npm install
call npm run build
cd ..
node scripts/embed_assets.js
cmake -B build -S .
cmake --build build --config Release
