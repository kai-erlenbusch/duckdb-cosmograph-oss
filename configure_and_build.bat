@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\exploratory\duckdb-extension\duckdb-cosmograph-oss
cmake -DEXTENSION_STATIC_BUILD=1 -DDUCKDB_EXTENSION_CONFIGS="D:\exploratory\duckdb-extension\duckdb-cosmograph-oss\extension_config.cmake" -DDUCKDB_EXPLICIT_PLATFORM="windows_amd64" -DCMAKE_BUILD_TYPE=Release -S duckdb -B build/release
cmake --build build/release --config Release
