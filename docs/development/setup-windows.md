# Hướng dẫn cài đặt môi trường phát triển C++ trên Windows

Tập tin này mô tả các bước để cài đặt các công cụ cần thiết để build dự án C++ này trên Windows (MSVC + CMake).

1) Yêu cầu tối thiểu

- Windows 10/11
- Git
- Visual Studio 2022 Build Tools (MSVC / C++ build tools)
- CMake (>= 3.25)
- Ninja (khuyên dùng)
- PowerShell (đã có sẵn trên Windows)

1) Cách cài (tùy chọn): sử dụng Chocolatey (khuyên dùng nếu bạn muốn script)

- Mở PowerShell dưới quyền Administrator và chạy (nếu chưa có Chocolatey, xem <https://chocolatey.org/install>):

```powershell
set-executionpolicy bypass -scope process -force
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

# Sau khi cài Chocolatey
choco install -y git cmake ninja
# Visual Studio Build Tools (cài MSVC toolset). Package name may change; nếu lỗi, cài thủ công từ trang Microsoft.
choco install -y visualstudio2022buildtools
```

1) Cài đặt thủ công (nếu không dùng Chocolatey)

- Visual Studio Build Tools: <https://visualstudio.microsoft.com/downloads/> → "Build Tools for Visual Studio 2022" → chọn workload "C++ build tools".
- CMake: <https://cmake.org/download/> (chọn add-to-path hoặc thêm thủ công)
- Ninja: <https://github.com/ninja-build/ninja/releases>

1) Mở workspace trong VS Code và các extension khuyến nghị

- Cài các extension: C/C++ (ms-vscode.cpptools), CMake Tools (ms-vscode.cmake-tools), CMake (twxs.cmake)
- Mở thư mục workspace (nơi chứa `CMakeLists.txt`).

1) Sử dụng CMake Presets (dự án đã có `CMakePresets.json`)

- Configure (cấu hình): từ Command Palette (Ctrl+Shift+P) chọn `CMake: Configure` hoặc chạy task `cmake: configure (msvc)` trong menu Task.
- Build: chạy task `cmake: build (msvc)` hoặc từ Command Palette chọn `CMake: Build`.
- Chạy test: chạy task `ctest: run (msvc)` hoặc mở terminal và chạy:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
```

1) Lưu ý về MSVC (`cl.exe`)

- Để dùng `cl.exe` bạn có thể mở "Developer Command Prompt for VS 2022" hoặc gọi `vcvarsall.bat`/`VsDevCmd.bat` để thiết lập environment. VS Code + CMake Tools thường xử lý tự động nếu Build Tools đã được cài.

1) Kiểm tra nhanh

- Kiểm tra các lệnh có trên PATH:

```powershell
git --version
cmake --version
ninja --version
cl.exe /?
```

1) Nếu cần tôi có thể:

- Tạo devcontainer để phát triển nhất quán (Linux container) — nói tôi biết nếu bạn muốn.
- Tự động hoá kiểm tra phụ thuộc bằng script PowerShell (đã thêm kèm theo repository).

---
File liên quan trong workspace:

- Task sẵn có: `cmake: configure (msvc)`, `cmake: build (msvc)`, `ctest: run (msvc)` (xem mục Tasks trong VS Code)

Nếu bạn muốn, tôi có thể tiếp tục và tạo devcontainer hoặc chạy kiểm tra phụ thuộc trên máy của bạn.
