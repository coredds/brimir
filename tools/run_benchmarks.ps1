#Requires -Version 5.0

<#
.SYNOPSIS
    Run Brimir benchmark suite and save results with timestamp

.DESCRIPTION
    Builds and runs all performance benchmarks, saving results
    with timestamp for tracking performance over time.

.PARAMETER Compare
    Compare with previous baseline (if exists)

.PARAMETER Save
    Save results as new baseline

.EXAMPLE
    .\tools\run_benchmarks.ps1
    Run benchmarks and display results

.EXAMPLE
    .\tools\run_benchmarks.ps1 -Save
    Run benchmarks and save as new baseline

.EXAMPLE
    .\tools\run_benchmarks.ps1 -Compare
    Run benchmarks and compare with previous baseline
#>

param(
    [switch]$Compare,
    [switch]$Save
)

$ErrorActionPreference = "Stop"

# Configuration
$BuildDir = "build"
$ResultsDir = "benchmark_results"
$BaselineFile = "$ResultsDir\baseline.txt"
$Timestamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$ResultFile = "$ResultsDir\benchmark_$Timestamp.txt"

# Create results directory if it doesn't exist
if (-not (Test-Path $ResultsDir)) {
    New-Item -ItemType Directory -Path $ResultsDir | Out-Null
    Write-Host "Created results directory: $ResultsDir" -ForegroundColor Green
}

# Banner
Write-Host "`n╔═══════════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║                                                                       ║" -ForegroundColor Cyan
Write-Host "║                 BRIMIR BENCHMARK SUITE                                ║" -ForegroundColor Cyan
Write-Host "║                                                                       ║" -ForegroundColor Cyan
Write-Host "╚═══════════════════════════════════════════════════════════════════════╝`n" -ForegroundColor Cyan

# Step 1: Build benchmark
Write-Host "Building benchmark_sh2..." -ForegroundColor Yellow
cmake --build $BuildDir --config Release --target benchmark_sh2 2>&1 | Out-Null

if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host "✅ Build successful`n" -ForegroundColor Green

# Step 2: Check if benchmark exists
$BenchmarkExe = "$BuildDir\bin\Release\Release\benchmark_sh2.exe"
if (-not (Test-Path $BenchmarkExe)) {
    Write-Host "❌ Benchmark executable not found: $BenchmarkExe" -ForegroundColor Red
    exit 1
}

# Step 3: Run benchmark
Write-Host "Running benchmarks (10M iterations each)..." -ForegroundColor Yellow
Write-Host "This may take 30-60 seconds...`n" -ForegroundColor Gray

$Output = & $BenchmarkExe

# Save results
$Output | Out-File -FilePath $ResultFile -Encoding UTF8
Write-Host "✅ Results saved to: $ResultFile`n" -ForegroundColor Green

# Display results
Write-Host $Output

# Step 4: Save as baseline if requested
if ($Save) {
    $Output | Out-File -FilePath $BaselineFile -Encoding UTF8
    Write-Host "`n✅ Results saved as baseline: $BaselineFile" -ForegroundColor Green
}

# Step 5: Compare with baseline if requested
if ($Compare) {
    if (Test-Path $BaselineFile) {
        Write-Host "`n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
        Write-Host "COMPARISON WITH BASELINE" -ForegroundColor Yellow
        Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
        
        $Baseline = Get-Content $BaselineFile
        $Current = Get-Content $ResultFile
        
        # Extract performance numbers (lines with "ns/op")
        $BaselinePerf = $Baseline | Select-String "ns/op"
        $CurrentPerf = $Current | Select-String "ns/op"
        
        Write-Host "`nPerformance changes:" -ForegroundColor White
        
        for ($i = 0; $i -lt [Math]::Min($BaselinePerf.Count, $CurrentPerf.Count); $i++) {
            $BaselineLine = $BaselinePerf[$i].Line
            $CurrentLine = $CurrentPerf[$i].Line
            
            # Extract numbers
            if ($BaselineLine -match "(\d+\.\d+) ns/op" -and $CurrentLine -match "(\d+\.\d+) ns/op") {
                $BaselineVal = [double]$Matches[1]
                $CurrentVal = [double]$Matches[1]
                
                # Extract benchmark name
                if ($CurrentLine -match "^(.+?)\s+\d+\.\d+") {
                    $Name = $Matches[1].Trim()
                    
                    if ($BaselineVal -ne 0) {
                        $PercentChange = (($CurrentVal - $BaselineVal) / $BaselineVal) * 100
                        
                        $Color = if ($PercentChange -le -5) { "Green" } 
                                elseif ($PercentChange -ge 5) { "Red" }
                                else { "Gray" }
                        
                        $Symbol = if ($PercentChange -le -5) { "✅" }
                                 elseif ($PercentChange -ge 5) { "⚠️" }
                                 else { "→" }
                        
                        Write-Host ("  {0} {1,-40} {2,6:F2} → {3,6:F2} ns/op ({4:+0.0;-0.0}%)" -f `
                            $Symbol, $Name, $BaselineVal, $CurrentVal, $PercentChange) -ForegroundColor $Color
                    }
                }
            }
        }
        
        Write-Host "`nLegend:" -ForegroundColor Gray
        Write-Host "  ✅ = Improvement (>5% faster)" -ForegroundColor Green
        Write-Host "  ⚠️  = Regression (>5% slower)" -ForegroundColor Red
        Write-Host "  → = No significant change" -ForegroundColor Gray
        
    } else {
        Write-Host "`n⚠️  No baseline found. Run with -Save to create baseline." -ForegroundColor Yellow
    }
}

# Summary
Write-Host "`n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
Write-Host "BENCHMARK COMPLETE" -ForegroundColor Green
Write-Host "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" -ForegroundColor Cyan
Write-Host "`nResults: $ResultFile" -ForegroundColor White
if ($Save) {
    Write-Host "Baseline: $BaselineFile (updated)" -ForegroundColor White
}
Write-Host "`n"

