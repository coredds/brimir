# Brimir Development Environment Setup Script
# Run this as Administrator in PowerShell

param(
    [switch]$Verify = $false
)

$ErrorActionPreference = "Continue"

# Colors for output
function Write-Success { Write-Host $args -ForegroundColor Green }
function Write-Info { Write-Host $args -ForegroundColor Cyan }
function Write-Warning { Write-Host $args -ForegroundColor Yellow }
function Write-Error { Write-Host $args -ForegroundColor Red }

# Banner
Write-Host ""
Write-Host "=========================================================" -ForegroundColor Magenta
Write-Host "       Brimir Development Environment Setup" -ForegroundColor Magenta
Write-Host "       Sega Saturn Emulation for libretro" -ForegroundColor Magenta
Write-Host "=========================================================" -ForegroundColor Magenta
Write-Host ""

# Check if running as admin
function Test-Administrator {
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($currentUser)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Verify tools
function Test-Tool {
    param([string]$Command, [string]$Name)
    
    Write-Info "Checking $Name..."
    try {
        $output = & $Command --version 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Success "  OK - $Name found"
            return $true
        }
    } catch {
        Write-Warning "  NOT FOUND - $Name"
        return $false
    }
    return $false
}

# Main verification function
function Invoke-Verification {
    Write-Info ""
    Write-Info "=== Verifying Development Environment ==="
    Write-Info ""
    
    $allFound = $true
    
    # Check Git
    if (-not (Test-Tool "git" "Git")) {
        $allFound = $false
    }
    
    # Check CMake
    if (-not (Test-Tool "cmake" "CMake")) {
        $allFound = $false
    }
    
    # Check Ninja
    if (-not (Test-Tool "ninja" "Ninja")) {
        $allFound = $false
    }
    
    # Check Python
    if (-not (Test-Tool "python" "Python")) {
        Write-Warning "  Python not found (optional but recommended)"
    }
    
    # Check for MSVC
    Write-Info "Checking MSVC Compiler..."
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -property installationPath
        if ($vsPath) {
            Write-Success "  OK - Visual Studio found"
            
            $vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $vcvarsall) {
                Write-Success "  OK - MSVC build tools available"
            } else {
                Write-Warning "  NOT FOUND - MSVC build tools"
                Write-Warning "    Install 'Desktop development with C++' workload"
                $allFound = $false
            }
        }
    } else {
        Write-Warning "  NOT FOUND - Visual Studio"
        $allFound = $false
    }
    
    Write-Host ""
    if ($allFound) {
        Write-Success "==========================================="
        Write-Success "ALL REQUIRED TOOLS ARE INSTALLED!"
        Write-Success "==========================================="
        Write-Info ""
        Write-Info "Next steps:"
        Write-Info "  1. Open 'Developer PowerShell for VS 2022'"
        Write-Info "  2. Navigate to your Brimir directory"
        Write-Info "  3. Continue with Phase 1 setup"
        Write-Info ""
    } else {
        Write-Error "==========================================="
        Write-Error "SOME TOOLS ARE MISSING!"
        Write-Error "==========================================="
        Write-Warning ""
        Write-Warning "Run this script without -Verify to install"
        Write-Warning ""
    }
    
    return $allFound
}

# Installation function
function Install-Tools {
    if (-not (Test-Administrator)) {
        Write-Error "This script requires Administrator privileges"
        Write-Info "Please run PowerShell as Administrator"
        exit 1
    }
    
    Write-Info ""
    Write-Info "=== Installing Development Tools ==="
    Write-Info ""
    
    # Check if winget is available
    try {
        $null = Get-Command winget -ErrorAction Stop
    } catch {
        Write-Error "winget not found!"
        Write-Info "Please update Windows or install winget manually"
        exit 1
    }
    
    # Install CMake
    Write-Info ""
    Write-Info "[1/3] Installing CMake..."
    try {
        winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements --silent
        Write-Success "  CMake installation completed"
    } catch {
        Write-Warning "  CMake installation may have failed"
    }
    
    # Install Ninja
    Write-Info ""
    Write-Info "[2/3] Installing Ninja..."
    try {
        winget install --id Ninja-build.Ninja -e --accept-package-agreements --accept-source-agreements --silent
        Write-Success "  Ninja installation completed"
    } catch {
        Write-Warning "  Ninja installation may have failed"
    }
    
    # Install Visual Studio Build Tools
    Write-Info ""
    Write-Info "[3/3] Installing Visual Studio Build Tools 2022..."
    Write-Warning ""
    Write-Warning "IMPORTANT: The Visual Studio Installer will open"
    Write-Warning "You MUST select: 'Desktop development with C++'"
    Write-Warning "Then click Install (~3-4GB download)"
    Write-Info ""
    Write-Info "Press Enter to continue..."
    Read-Host
    
    try {
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements
        Write-Success "  Visual Studio installer started"
    } catch {
        Write-Warning "  Installation may have failed"
    }
    
    Write-Info ""
    Write-Info "==========================================="
    Write-Success "Installation commands completed!"
    Write-Info "==========================================="
    
    Write-Warning ""
    Write-Warning "Next steps:"
    Write-Info "  1. Complete Visual Studio installation"
    Write-Info "  2. Close ALL PowerShell windows"
    Write-Info "  3. Open new PowerShell"
    Write-Info "  4. Run: .\setup-env.ps1 -Verify"
    Write-Info ""
}

# Main script logic
if ($Verify) {
    Invoke-Verification
} else {
    Write-Info "This script will install:"
    Write-Info "  - CMake (build system)"
    Write-Info "  - Ninja (build tool)"
    Write-Info "  - Visual Studio Build Tools 2022"
    Write-Info ""
    Write-Info "Total download: ~3-4 GB"
    Write-Info "Time required: ~30-60 minutes"
    Write-Info ""
    
    $continue = Read-Host "Continue? (Y/N)"
    if ($continue -eq "Y" -or $continue -eq "y") {
        Install-Tools
    } else {
        Write-Info "Installation cancelled"
    }
}
