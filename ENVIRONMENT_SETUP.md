# Development Environment Setup - Windows

## Current Environment Status

### ✅ Installed
- **Git**: v2.43.0.windows.1 ✓
- **Python**: v3.12.0 ✓
- **PowerShell**: v7 ✓
- **OS**: Windows 10 Build 26200 ✓

### ❌ Missing Required Tools
- **CMake**: Not found (Required: 3.20+)
- **C++ Compiler**: Not found (Required: MSVC 2022, GCC 11+, or Clang 14+)
- **Ninja** (Optional but recommended): Not found

---

## Installation Guide

### 1. Install Visual Studio 2022 (Recommended for Windows)

**Option A: Visual Studio Community 2022 (Full IDE)**
1. Download from: https://visualstudio.microsoft.com/downloads/
2. Run installer
3. Select workload: **"Desktop development with C++"**
4. Individual components to ensure are selected:
   - MSVC v143 - VS 2022 C++ x64/x86 build tools
   - C++ CMake tools for Windows
   - C++20 standard library modules
   - Windows 10 or 11 SDK
5. Install (requires ~7GB)

**Option B: Build Tools for Visual Studio 2022 (Command-line only, smaller)**
1. Download from: https://visualstudio.microsoft.com/downloads/ (scroll to "Tools for Visual Studio")
2. Select "Build Tools for Visual Studio 2022"
3. Select: **"Desktop development with C++"**
4. Install (requires ~3-4GB)

### 2. Install CMake

**Option A: Via Visual Studio Installer**
- If using Visual Studio, CMake is included in "C++ CMake tools for Windows"

**Option B: Standalone Installation**
1. Download from: https://cmake.org/download/
2. Get the Windows x64 Installer (cmake-3.XX.X-windows-x86_64.msi)
3. During installation, select: **"Add CMake to the system PATH for all users"**
4. Install

**Option C: Via Chocolatey (if you use it)**
```powershell
choco install cmake
```

**Option D: Via winget**
```powershell
winget install Kitware.CMake
```

### 3. Install Ninja (Optional but Recommended)

Ninja provides faster builds than MSBuild.

**Option A: Via Visual Studio**
- Included with "C++ CMake tools for Windows"

**Option B: Via Chocolatey**
```powershell
choco install ninja
```

**Option C: Via winget**
```powershell
winget install Ninja-build.Ninja
```

**Option D: Manual**
1. Download from: https://github.com/ninja-build/ninja/releases
2. Extract ninja.exe
3. Add to PATH

### 4. Verify Installation

After installation, **restart your terminal** and run:

```powershell
# Check CMake
cmake --version
# Should show: cmake version 3.XX.X or higher

# Check MSVC (need to use Developer Command Prompt or PowerShell)
# From regular PowerShell, try:
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
cl
# Should show: Microsoft (R) C/C++ Optimizing Compiler Version XX.XX

# Check Ninja (if installed)
ninja --version
# Should show: 1.XX.X
```

---

## Quick Install Commands (Recommended)

If you have **winget** (Windows Package Manager):

```powershell
# Install CMake
winget install Kitware.CMake

# Install Visual Studio Build Tools 2022
winget install Microsoft.VisualStudio.2022.BuildTools

# Then configure Build Tools to include C++ workload
# (This will open the Visual Studio Installer)
```

---

## Alternative: Using LLVM/Clang on Windows

If you prefer Clang over MSVC:

1. **Install LLVM**:
   ```powershell
   winget install LLVM.LLVM
   ```

2. **Install Ninja and CMake** (as above)

3. Clang will be added to PATH automatically

---

## After Installation Setup

### 1. Set up Visual Studio Developer Environment

**Option A: Use Developer PowerShell**
- Start Menu → "Developer PowerShell for VS 2022"

**Option B: Initialize in Current PowerShell**
```powershell
# Add to your PowerShell profile for automatic loading
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"
```

**Option C: Use vcvarsall.bat**
```cmd
"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
```

### 2. Verify C++20 Support

```powershell
# Create test file
echo '#include <iostream>' > test.cpp
echo 'int main() { std::cout << __cplusplus; }' >> test.cpp

# Compile with C++20
cl /std:c++20 test.cpp

# Run
.\test.exe
# Should output: 202002 or higher

# Cleanup
Remove-Item test.cpp, test.exe, test.obj
```

---

## Optional Tools (Recommended)

### Git GUI Tools
- **GitHub Desktop**: Easy git management
- **GitKraken**: Visual git client
- **VS Code**: Excellent git integration

### Code Editors
- **Visual Studio 2022**: Full IDE with debugger
- **VS Code**: Lightweight, great CMake extension
- **CLion**: JetBrains IDE (paid)

### Useful VS Code Extensions
```
ms-vscode.cpptools
ms-vscode.cmake-tools
twxs.cmake
ms-vscode.powershell
```

---

## Troubleshooting

### "cmake not found" after installation
- **Solution**: Restart terminal or add to PATH manually
- Check: `C:\Program Files\CMake\bin`

### "cl is not recognized"
- **Solution**: Use Developer PowerShell or run Launch-VsDevShell.ps1
- Alternative: Add MSVC to PATH (complex, not recommended)

### "Cannot find Visual Studio"
- **Solution**: Ensure "Desktop development with C++" workload installed
- Repair installation via Visual Studio Installer

### Build errors about C++20
- **Solution**: Ensure MSVC v143 (VS 2022) tools are installed
- Update Visual Studio to latest version

---

## Estimated Download Sizes

- **Visual Studio 2022 Community**: ~7 GB
- **Visual Studio Build Tools**: ~3-4 GB  
- **CMake**: ~40 MB
- **Ninja**: ~1 MB
- **LLVM/Clang**: ~350 MB

---

## Next Steps After Setup

Once all tools are installed:

1. ✅ Verify all tools work
2. ✅ Clone Ymir as submodule
3. ✅ Clone vcpkg as submodule
4. ✅ Set up CMake configuration
5. ✅ Create initial build
6. ✅ Begin Phase 1 Week 1 tasks

See [QUICKSTART.md](QUICKSTART.md) for build instructions.

---

## Quick Reference Commands

```powershell
# Check environment
git --version
cmake --version
cl  # (in Developer PowerShell)
ninja --version

# Initialize VS environment (if needed)
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1"

# Alternative paths for Build Tools
& "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1"
```

---

**Ready to install?** Start with Visual Studio and CMake - those are the essentials!

