$env:VSCMD_DEBUG=1
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake -B build -S . && cmake --build build --config Release"
