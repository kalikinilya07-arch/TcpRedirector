# Сборка TcpRedirector v1.1.0

## Требования

- Visual Studio 2022 (с C++ и .NET workload)
- CMake 3.20+
- .NET 9 SDK
- Inno Setup 6 (для сборки инсталлятора)
- vcpkg (для nlohmann_json)

## Сборка службы (C++)

```bat
cd src\service
build.bat
```

Выходные файлы: `build/Release/TcpRedirectorService.exe`

## Сборка AuthAgent (C++)

```bat
cd src\auth-agent\TcpRedirectorAuthAgent
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

Выходные файлы: `build/Release/TcpRedirectorAuthAgent.exe`

## Сборка GUI (.NET)

```bat
cd src\gui\TcpRedirectorGUI
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Выходные файлы: `bin/Release/net9.0-windows/win-x64/publish/TcpRedirectorGUI.exe`

## Сборка инсталлятора

```bat
iscc installer\TcpRedirectorSetup.iss
```

Выходные файлы: `release/1.1.0/installer/TcpRedirectorSetup-1.1.0.exe`

## Копирование в релизную директорию

```bat
mkdir release\1.1.0\bin
copy src\service\build\Release\TcpRedirectorService.exe release\1.1.0\bin\
copy src\auth-agent\TcpRedirectorAuthAgent\build\Release\TcpRedirectorAuthAgent.exe release\1.1.0\bin\
copy src\gui\TcpRedirectorGUI\bin\Release\net9.0-windows\win-x64\publish\TcpRedirectorGUI.exe release\1.1.0\bin\
copy deploy\WinDivert.dll release\1.1.0\bin\
copy deploy\WinDivert64.sys release\1.1.0\bin\
copy deploy\WinDivertInstall.exe release\1.1.0\bin\
copy deploy\install_windivert.bat release\1.1.0\bin\
copy docs\* release\1.1.0\docs\
copy CHANGELOG.md release\1.1.0\
