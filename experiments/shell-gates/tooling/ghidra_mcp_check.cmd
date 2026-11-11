@echo off
rem Checks the GhidraMCP plugin HTTP server (Ghidra GUI must be running with GhidraMCPPlugin enabled).
curl -s http://127.0.0.1:8089/check_connection
echo.
curl -s http://127.0.0.1:8089/mcp/health
echo.
