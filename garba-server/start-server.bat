@echo off
title Garba Step Counter Server
color 0A
echo.
echo ========================================
echo    🎪 GARBA STEP COUNTER SERVER 🎪
echo ========================================
echo.
echo Starting server...
echo.
echo Dashboard will be available at:
echo   http://garba.local:3000/stepcounter
echo   http://localhost:3000/stepcounter
echo.
echo Debug Console:
echo   http://garba.local:3000/debugger
echo.
echo Press Ctrl+C to stop the server
echo ========================================
echo.

npm start

pause