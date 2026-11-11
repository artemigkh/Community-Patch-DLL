@echo off
rem RenderDoc 1.47 portable. Capture of a 32-bit app uses the x86\ layer automatically from the 64-bit UI.
rem   renderdoc.cmd                  (qrenderdoc UI)
rem   renderdoc.cmd cmd --version    (renderdoccmd)
if /i "%~1"=="cmd" (shift & "C:\Users\Art\tools\renderdoc-1.47\renderdoccmd.exe" %2 %3 %4 %5 %6 %7 %8 %9 & goto :eof)
start "" "C:\Users\Art\tools\renderdoc-1.47\qrenderdoc.exe" %*
