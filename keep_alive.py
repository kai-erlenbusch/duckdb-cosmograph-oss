import subprocess
import time

print("Starting server...")
p = subprocess.Popen(
    ['build/release/Release/duckdb.exe', '-init', 'run_server.sql'],
    stdin=subprocess.PIPE
)

print("Server is running. Keeping alive...")
try:
    time.sleep(86400)
except KeyboardInterrupt:
    p.terminate()
