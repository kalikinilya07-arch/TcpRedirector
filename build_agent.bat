@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "C:\Users\user\Desktop\TcpRedirector\src\auth-agent\TcpRedirectorAuthAgent"
cl /EHsc /std:c++17 /DUNICODE /D_UNICODE /DSECURITY_WIN32 /DWIN32_LEAN_AND_MEAN /IC:\Users\user\Desktop\TcpRedirector\external /IC:\Users\user\Desktop\TcpRedirector\external\nlohmann /Fe:TcpRedirectorAuthAgent.exe main.cpp SspiEngine.cpp ContextStore.cpp /link secur32.lib advapi32.lib wtsapi32.lib userenv.lib /SUBSYSTEM:CONSOLE