@echo off
rem Starts the CYD News bridge. Leave this window open while the CYD is in use.
cd /d "%~dp0"
python -m pip install -q -r requirements.txt
python server.py --port 8080
pause
