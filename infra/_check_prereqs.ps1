<#
Simple prereq checker for the C++ development environment.
Run from PowerShell: `.	ools\_check_prereqs.ps1` or `infra\_check_prereqs.ps1`
#>

function Check-Cmd {
    param($name, $displayName)
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Host "$displayName: FOUND -> $($cmd.Source)" -ForegroundColor Green
    } else {
        Write-Host "$displayName: NOT FOUND" -ForegroundColor Yellow
    }
}

Write-Host "Checking required CLI tools for this project..." -ForegroundColor Cyan
Check-Cmd git "Git"
Check-Cmd cmake "CMake"
Check-Cmd ninja "Ninja"
Check-Cmd cl.exe "MSVC cl.exe"
Check-Cmd ctest "CTest"

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "Note: 'cl.exe' thường chỉ có khi bạn mở Developer Command Prompt for VS hoặc sau khi chạy VsDevCmd.bat. Nếu bạn đã cài Visual Studio Build Tools nhưng không thấy cl.exe, mở 'x64 Native Tools Command Prompt for VS 2022' và thử lại." -ForegroundColor Yellow
}

Write-Host "
If some tools are missing, consider installing via Chocolatey (run PowerShell as Admin):`choco install -y git cmake ninja visualstudio2022buildtools`" -ForegroundColor Magenta
