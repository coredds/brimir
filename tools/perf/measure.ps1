<#
.SYNOPSIS
  Times two builds on the recon windows the recon way: rounds as the outer loop, A and B interleaved inside each round
  (order alternating per round), one run at a time. Reports per window and mode the median ms/frame of each build, the
  max-min spread across rounds, the B-vs-A change, and the stage split brimir_bench reports (Ymir_RunFrame, SH-2
  master/slave, the rest). Writes measure.csv (every run) and measure.md (medians).
.EXAMPLE
  tools\perf\measure.ps1 -A $env:TEMP\opencode\perf-base-f395c6f\build-lto\bin -B .\build-lto\bin
#>
param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B,
    [int]$Rounds = 3,
    [int]$Frames = 1200,
    [string]$Windows = '',
    [string]$Modes = 'thr,unthr',
    [string[]]$Extra = @(),
    [string]$Records = $(if ($env:BRIMIR_PERF_RECORDS) { $env:BRIMIR_PERF_RECORDS } else { 'C:\Users\david\projects\Brimir\docs\superpowers\records\2026-11-11-perf-recon' }),
    [string]$Roms = 'F:\OneDrive\Roms\saturn',
    [string]$BiosDir = 'F:\OneDrive\Roms\BIOS',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$windowList = Select-PerfWindows $Windows
$modeList = Select-PerfModes $Modes
Assert-PerfInputs $Records $Roms $BiosDir $windowList
$bench = @{ A = Get-PerfBench $A; B = Get-PerfBench $B }
if (-not $Out) { $Out = Join-Path $PerfScratch ("perf-measure\" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out, (Join-Path $Out 'logs') | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path

$prov = [ordered]@{ A = Get-PerfProvenance $A; B = Get-PerfProvenance $B }
$prov | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'provenance.json') -Encoding utf8
Write-Host "A: $($prov.A.commit)  bench $($prov.A.benchSha256.Substring(0, 16))  ($A)"
Write-Host "B: $($prov.B.commit)  bench $($prov.B.benchSha256.Substring(0, 16))  ($B)"
Write-Host "out: $Out"

$csv = Join-Path $Out 'measure.csv'
'round,window,mode,build,avg,p50,p95,p99,max,ymir,master,slave,exit' | Set-Content $csv
$rows = [System.Collections.Generic.List[object]]::new()
$sw = [Diagnostics.Stopwatch]::StartNew()
$sys = Join-Path $Out 'sys'
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($w in $windowList) {
        foreach ($m in $modeList) {
            $order = if ($r % 2) { 'A', 'B' } else { 'B', 'A' }
            foreach ($side in $order) {
                $bios = New-PerfSysDir $sys $w $Records $BiosDir
                $benchArgs = (Get-PerfBenchArgs $w $sys $bios $Records $Roms) + @('--frames', "$Frames") + $PerfModes[$m] + $Extra
                $log = Join-Path $Out "logs\$w-$m-$side-r$r.txt"
                $exe = $bench[$side]
                & $exe @benchArgs *> $log
                $code = $LASTEXITCODE
                $t = Get-Content -LiteralPath $log -Raw
                $mf = [regex]::Match($t, 'ms/frame\s+: avg ([\d.]+)\s+p50 ([\d.]+)\s+p95 ([\d.]+)\s+p99 ([\d.]+)\s+max ([\d.]+)')
                $num = { param($pattern) $x = [regex]::Match($t, $pattern); if ($x.Success) { [double]$x.Groups[1].Value } else { [double]::NaN } }
                $row = [pscustomobject]@{
                    round = $r; window = $w; mode = $m; build = $side
                    avg = if ($mf.Success) { [double]$mf.Groups[1].Value } else { [double]::NaN }
                    p50 = if ($mf.Success) { [double]$mf.Groups[2].Value } else { [double]::NaN }
                    p95 = if ($mf.Success) { [double]$mf.Groups[3].Value } else { [double]::NaN }
                    p99 = if ($mf.Success) { [double]$mf.Groups[4].Value } else { [double]::NaN }
                    max = if ($mf.Success) { [double]$mf.Groups[5].Value } else { [double]::NaN }
                    ymir = & $num 'Ymir_RunFrame: ([\d.]+)'
                    master = & $num 'SH2 master\s+: ([\d.]+)'
                    slave = & $num 'SH2 slave\s+: ([\d.]+)'
                    exit = $code
                }
                $rows.Add($row)
                ($row.PSObject.Properties.Value -join ',') | Add-Content $csv
                Write-Host ("r{0} {1,-8} {2,-5} {3}: {4,7:n3} ms/frame{5}" -f $r, $w, $m, $side, $row.avg, $(if ($code) { "  EXIT $code" } else { '' }))
            }
        }
    }
}
Remove-Item -LiteralPath $sys -Recurse -Force -ErrorAction SilentlyContinue
$sw.Stop()

$summary = foreach ($w in $windowList) {
    foreach ($m in $modeList) {
        $s = [ordered]@{ window = $w; mode = $m }
        foreach ($side in 'A', 'B') {
            $set = @($rows | Where-Object { $_.window -eq $w -and $_.mode -eq $m -and $_.build -eq $side -and $_.exit -eq 0 })
            $avg = [double[]]@($set.avg)
            $med = Get-PerfMedian $avg
            $s["${side}_avg"] = $med
            $s["${side}_spread"] = if ($set.Count -gt 1 -and $med -gt 0) { 100 * (($avg | Measure-Object -Maximum).Maximum - ($avg | Measure-Object -Minimum).Minimum) / $med } else { [double]::NaN }
            $s["${side}_p95"] = Get-PerfMedian ([double[]]@($set.p95))
            $s["${side}_ymir"] = Get-PerfMedian ([double[]]@($set.ymir))
            $s["${side}_sh2"] = Get-PerfMedian ([double[]]@($set | ForEach-Object { $_.master + $_.slave }))
            $s["${side}_other"] = Get-PerfMedian ([double[]]@($set | ForEach-Object { $_.ymir - $_.master - $_.slave }))
            $s["${side}_runs"] = $set.Count
        }
        $s.delta = 100 * ($s.B_avg - $s.A_avg) / $s.A_avg
        $noise = 3.0
        foreach ($sp in $s.A_spread, $s.B_spread) { if (-not [double]::IsNaN($sp)) { $noise = [math]::Max($noise, $sp) } }
        $s.verdict = if ([double]::IsNaN($s.delta)) { 'n/a' } elseif ([math]::Abs($s.delta) -le $noise) { 'noise' } elseif ($s.delta -lt 0) { 'faster' } else { 'SLOWER' }
        foreach ($key in @($s.Keys)) { if ($s[$key] -is [double]) { $s[$key] = [math]::Round($s[$key], 3) } }
        [pscustomobject]$s
    }
}
$summary | Export-Csv (Join-Path $Out 'measure-summary.csv') -NoTypeInformation
$f = { param($v) if ([double]::IsNaN($v)) { '-' } else { '{0:n2}' -f $v } }
$md = @("# measure: A $($prov.A.commit) vs B $($prov.B.commit)", '',
    "$Rounds rounds interleaved (A,B then B,A), $Frames frames per window$(if ($Extra) { ", extra: $($Extra -join ' ')" }); wall $([int]$sw.Elapsed.TotalMinutes) min. ms/frame medians; spread = (max - min) / median of the rounds; verdict: 'noise' when |change| <= max(3 %, both spreads). Stages: Ymir_RunFrame = SH-2 (master + slave) + other.", '',
    '| window | mode | A avg | A spread % | B avg | B spread % | change % | verdict | A p95 | B p95 | A SH-2 / other | B SH-2 / other |',
    '|---|---|---|---|---|---|---|---|---|---|---|---|')
$md += $summary | ForEach-Object {
    "| $($_.window) | $($_.mode) | $(& $f $_.A_avg) | $(& $f $_.A_spread) | $(& $f $_.B_avg) | $(& $f $_.B_spread) | $(& $f $_.delta) | $($_.verdict) | $(& $f $_.A_p95) | $(& $f $_.B_p95) | $(& $f $_.A_sh2) / $(& $f $_.A_other) | $(& $f $_.B_sh2) / $(& $f $_.B_other) |"
}
$md | Set-Content (Join-Path $Out 'measure.md') -Encoding utf8
$md | Write-Host
Write-Host ("{0} runs in {1:n1} min; results in {2}" -f $rows.Count, $sw.Elapsed.TotalMinutes, $Out)
if (@($rows | Where-Object exit -ne 0).Count) { Write-Host 'Some runs failed (see logs)' -ForegroundColor Red; exit 2 }
exit 0
