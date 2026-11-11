<#
.SYNOPSIS
  Release+LTO build (as release.yml) of a commit or a source tree, with provenance. Prints the bin directory last.
.EXAMPLE
  tools\perf\baseline.ps1 -Commit f395c6f
  # -> %TEMP%\opencode\perf-base-f395c6f\build-lto\bin (a detached git worktree; reused when already built)
.EXAMPLE
  tools\perf\baseline.ps1 -Tree .
  # -> .\build-lto\bin: the working tree as it is (uncommitted changes included and recorded in provenance.json)
#>
[CmdletBinding(DefaultParameterSetName = 'Commit')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Commit')][string]$Commit,
    [Parameter(Mandatory, ParameterSetName = 'Tree')][string]$Tree,
    [string[]]$Targets = @('brimir_bench', 'brimir_state_diff', 'brimir_libretro', 'brimir_vkhost'),
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$vsrun = Join-Path $PerfScratch 'vsrun.cmd'
if (-not (Test-Path -LiteralPath $vsrun)) { throw "Missing $vsrun (runs a command inside the VS x64 developer environment)" }

if ($PSCmdlet.ParameterSetName -eq 'Commit') {
    $full = (git -C $PerfRepo rev-parse --verify "$Commit^{commit}").Trim()
    if ($LASTEXITCODE -ne 0) { throw "Unknown commit: $Commit" }
    $src = Join-Path $PerfScratch "perf-base-$($full.Substring(0, 7))"
    if (-not (Test-Path -LiteralPath (Join-Path $src '.git'))) {
        git -C $PerfRepo worktree add --detach $src $full | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed" }
    }
    $head = (git -C $src rev-parse HEAD).Trim()
    if ($head -ne $full) { throw "$src is at $head, not ${full}: remove it (git worktree remove) and rerun" }
} else {
    $src = (Resolve-Path -LiteralPath $Tree).Path
}

$build = Join-Path $src 'build-lto'
$bin = Join-Path $build 'bin'
$prov = Join-Path $bin 'provenance.json'
$commitNow = (git -C $src rev-parse HEAD).Trim()
$statusNow = (git -C $src status --porcelain) -join "`n"

if (-not $Rebuild -and $PSCmdlet.ParameterSetName -eq 'Commit' -and (Test-Path -LiteralPath $prov)) {
    $p = Get-Content -LiteralPath $prov -Raw | ConvertFrom-Json
    $ok = $p.commit -eq $commitNow -and -not $p.status
    foreach ($t in $Targets) {
        $f = Get-ChildItem -LiteralPath $bin -File -Filter "$t.*" -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.exe', '.dll' } | Select-Object -First 1
        if (-not $f -or $p.files.($f.Name) -ne (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash) { $ok = $false }
    }
    if ($ok) {
        Write-Host "Reusing $bin (built $($p.built) at $($p.commit))"
        Write-Output $bin
        return
    }
}

$cmakeArgs = @('-DCMAKE_BUILD_TYPE=Release', '-DBRIMIR_LTO=ON')
$sw = [Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force $build | Out-Null
$log = Join-Path $build 'baseline-build.log'
cmd /c "`"$vsrun`" cmake -S `"$src`" -B `"$build`" -G Ninja $($cmakeArgs -join ' ')" *> $log
if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30 | Out-Host; throw "cmake configure failed (log: $log)" }
cmd /c "`"$vsrun`" cmake --build `"$build`" --target $($Targets -join ' ')" *>> $log
if ($LASTEXITCODE -ne 0) {
    Get-Content $log | Select-String -Pattern 'error|FAILED' | Select-Object -First 30 | Out-Host
    throw "build failed (log: $log)"
}
$sw.Stop()

$files = [ordered]@{}
foreach ($t in $Targets) {
    $f = Get-ChildItem -LiteralPath $bin -File -Filter "$t.*" | Where-Object { $_.Extension -in '.exe', '.dll' } | Select-Object -First 1
    if (-not $f) { throw "Target $t produced nothing in $bin" }
    $files[$f.Name] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
}
$compiler = (Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^CMAKE_CXX_COMPILER:' | Select-Object -First 1).Line
[ordered]@{
    commit       = $commitNow
    status       = $statusNow
    source       = $src
    cmake        = ($cmakeArgs + '-G Ninja') -join ' '
    compiler     = $compiler
    built        = (Get-Date).ToString('s')
    buildSeconds = [int]$sw.Elapsed.TotalSeconds
    files        = $files
} | ConvertTo-Json | Set-Content -LiteralPath $prov -Encoding utf8
if ($statusNow) { Write-Warning "Built with uncommitted changes (recorded in provenance.json):`n$statusNow" }
Write-Host "Built $bin in $([int]$sw.Elapsed.TotalMinutes) min"
Write-Output $bin
