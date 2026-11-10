@echo off
rem Ghidra 12.1.4 GUI on Temurin 21 (launch.properties also pins JAVA_HOME_OVERRIDE). Opens the last project.
set "JAVA_HOME=C:\Users\Art\tools\temurin-jdk-21.0.13+9"
start "" "C:\Users\Art\tools\ghidra-12.1.4\ghidraRun.bat"
