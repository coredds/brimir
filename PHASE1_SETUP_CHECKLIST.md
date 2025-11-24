# Phase 1 Setup Checklist

This is your step-by-step guide to get started with Brimir Phase 1 development.

---

## ✅ What You Have

- **Git**: v2.43.0.windows.1 ✓
- **Python**: v3.12.0 ✓
- **PowerShell**: v7 ✓
- **Windows 10**: Build 26200 ✓

---

## ❌ What You Need to Install

### Critical Requirements

#### 1. CMake (3.20 or newer)
**Why:** Build system for the project  
**Status:** ❌ Not installed  
**Install:** 
```powershell
# Easiest method - using winget:
winget install Kitware.CMake

# Or download from: https://cmake.org/download/
```

#### 2. C++20 Compiler
**Why:** Brimir requires C++20 features (inherited from Ymir)  
**Status:** ❌ Not installed  
**Options:**

**Option A - MSVC (Recommended for Windows):**
```powershell
# Using winget:
winget install Microsoft.VisualStudio.2022.BuildTools

# Then in Visual Studio Installer, select:
# - Desktop development with C++
# - MSVC v143 - VS 2022 C++ build tools
# - C++20 standard library modules
```

**Option B - LLVM/Clang:**
```powershell
winget install LLVM.LLVM
```

#### 3. Ninja Build System (Recommended)
**Why:** Faster builds than MSBuild  
**Status:** ❌ Not installed  
**Install:**
```powershell
winget install Ninja-build.Ninja
```

---

## 📋 Installation Steps

### Step 1: Install Core Tools (30-60 minutes)

Run these commands in PowerShell as Administrator:

```powershell
# Install CMake
winget install Kitware.CMake

# Install Visual Studio Build Tools 2022
winget install Microsoft.VisualStudio.2022.BuildTools

# Install Ninja
winget install Ninja-build.Ninja
```

After running the Visual Studio Build Tools installer:
1. Select **"Desktop development with C++"** workload
2. Ensure these components are checked:
   - MSVC v143 - VS 2022 C++ x64/x86 build tools
   - C++ CMake tools for Windows
   - C++20 standard library modules
   - Windows 10 or 11 SDK
3. Click Install (requires 3-4 GB download)

### Step 2: Verify Installation

**Close and reopen PowerShell**, then run:

```powershell
# Check Git (should already work)
git --version
# Expected: git version 2.43.0.windows.1

# Check CMake
cmake --version
# Expected: cmake version 3.XX.X

# Check Ninja
ninja --version
# Expected: 1.XX.X

# Check C++ compiler (need Developer PowerShell)
# Option A: Open "Developer PowerShell for VS 2022" from Start Menu
# Option B: Run this in your current PowerShell:
& "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1"

# Then test compiler:
cl
# Expected: Microsoft (R) C/C++ Optimizing Compiler Version XX.XX.XXXXX
```

### Step 3: Test C++20 Support

```powershell
# Create a test file
@"
#include <iostream>
#include <concepts>

template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

int main() {
    std::cout << "C++20 Standard: " << __cplusplus << std::endl;
    return 0;
}
"@ | Out-File -FilePath test.cpp -Encoding ASCII

# Compile with C++20
cl /std:c++20 /EHsc test.cpp

# Run it
.\test.exe
# Should output: C++20 Standard: 202002

# Cleanup
Remove-Item test.cpp, test.exe, test.obj
```

If this works, you're ready! ✅

---

## 🚀 Once Environment is Ready

### Phase 1 Week 1 Tasks

After verifying your environment, you'll proceed with:

1. **Create project structure**
   - Set up source directories
   - Create include directories
   - Set up test framework structure

2. **Initialize Git submodules**
   - Add Ymir as submodule: `git submodule add https://github.com/StrikerX3/Ymir.git src/ymir`
   - Add vcpkg as submodule: `git submodule add https://github.com/microsoft/vcpkg.git vcpkg`
   - Initialize: `git submodule update --init --recursive`

3. **Create CMake configuration**
   - Root CMakeLists.txt
   - CMakePresets.json
   - Configure Ymir integration

4. **Create Makefile.libretro**
   - For libretro buildbot compatibility

5. **Set up CI/CD**
   - GitHub Actions workflows for Windows/Linux/macOS

---

## 🔧 Recommended Optional Tools

### VS Code (Excellent for CMake projects)
```powershell
winget install Microsoft.VisualStudioCode
```

**Useful Extensions:**
- C/C++ (ms-vscode.cpptools)
- CMake Tools (ms-vscode.cmake-tools)
- CMake (twxs.cmake)

### Visual Studio Community (Full IDE)
If you prefer a full IDE instead of just build tools:
```powershell
winget install Microsoft.VisualStudio.2022.Community
```

### GitHub Desktop (If you prefer GUI for Git)
```powershell
winget install GitHub.GitHubDesktop
```

---

## ⚡ Quick Install Script

Want to install everything at once? Save this as `setup-env.ps1` and run it:

```powershell
# setup-env.ps1
Write-Host "Installing Brimir development environment..." -ForegroundColor Green

# Check if running as admin
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Please run this script as Administrator" -ForegroundColor Red
    exit 1
}

Write-Host "Installing CMake..." -ForegroundColor Yellow
winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements

Write-Host "Installing Ninja..." -ForegroundColor Yellow
winget install --id Ninja-build.Ninja -e --accept-package-agreements --accept-source-agreements

Write-Host "Installing Visual Studio Build Tools 2022..." -ForegroundColor Yellow
Write-Host "NOTE: After installer opens, select 'Desktop development with C++'" -ForegroundColor Cyan
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements

Write-Host "`nInstallation commands sent!" -ForegroundColor Green
Write-Host "After Visual Studio Installer opens:" -ForegroundColor Yellow
Write-Host "  1. Select 'Desktop development with C++'" -ForegroundColor White
Write-Host "  2. Click Install" -ForegroundColor White
Write-Host "  3. Wait for completion (~3-4GB download)" -ForegroundColor White
Write-Host "`nAfter installation completes:" -ForegroundColor Yellow
Write-Host "  1. Close this PowerShell window" -ForegroundColor White
Write-Host "  2. Open Developer PowerShell for VS 2022" -ForegroundColor White
Write-Host "  3. Navigate to your project directory" -ForegroundColor White
Write-Host "  4. Run verification commands" -ForegroundColor White
```

To use it:
```powershell
# Run as Administrator
powershell -ExecutionPolicy Bypass -File setup-env.ps1
```

---

## 📚 Documentation References

Once your environment is ready:
- [QUICKSTART.md](QUICKSTART.md) - Build and development guide
- [ROADMAP.md](ROADMAP.md) - Phase 1 detailed tasks
- [ARCHITECTURE.md](ARCHITECTURE.md) - System design
- [ENVIRONMENT_SETUP.md](ENVIRONMENT_SETUP.md) - Detailed setup guide

---

## ❓ Common Issues

### "winget not found"
- Update Windows to latest version
- Or download tools manually from websites

### "cmake not found" after install
- Restart PowerShell
- Or log out/log in to refresh PATH

### "cl not recognized"
- Use "Developer PowerShell for VS 2022" from Start Menu
- Or run: `& "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1"`

### Visual Studio Installer won't start
- Download manually: https://visualstudio.microsoft.com/downloads/
- Select "Build Tools for Visual Studio 2022"

---

## 📊 Timeline Estimate

| Task | Time | Status |
|------|------|--------|
| Install CMake | 5 min | ⏳ Pending |
| Install Ninja | 2 min | ⏳ Pending |
| Install VS Build Tools | 30-60 min | ⏳ Pending |
| Verify installation | 5 min | ⏳ Pending |
| **Total** | **~45-75 min** | |

After this, you'll be ready to start actual development!

---

## ✅ Checklist

Use this to track your progress:

- [ ] CMake installed and in PATH
- [ ] Ninja installed and in PATH  
- [ ] Visual Studio Build Tools 2022 installed
- [ ] "Desktop development with C++" workload selected
- [ ] MSVC v143 toolset installed
- [ ] C++20 support verified
- [ ] Developer PowerShell accessible
- [ ] All verification commands pass
- [ ] Test C++20 compilation successful

**When all boxes are checked, you're ready for Phase 1 Week 1! 🎉**

---

## 🆘 Need Help?

If you run into issues:
1. Check [ENVIRONMENT_SETUP.md](ENVIRONMENT_SETUP.md) for detailed troubleshooting
2. Create an issue on GitHub
3. Check Visual Studio documentation
4. CMake documentation: https://cmake.org/documentation/

---

**Ready to install?** Start with the commands in Step 1 above! The whole process takes about 1 hour.

