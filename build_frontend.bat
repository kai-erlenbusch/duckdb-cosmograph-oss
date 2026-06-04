@echo off
echo Building frontend...
cd frontend
call npm install
call npm run build
if %errorlevel% neq 0 exit /b %errorlevel%

echo Embedding assets...
cd ..
node scripts\embed_assets.js
if %errorlevel% neq 0 exit /b %errorlevel%

echo Frontend build and asset embedding complete!
