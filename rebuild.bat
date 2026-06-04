cd /d D:\exploratory\duckdb-extension\duckdb-cosmograph-oss
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build/release -S .
cmake --build build/release --config Release
