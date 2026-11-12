<#
.SYNOPSIS
  Times two builds on the recon windows with paired, interleaved rounds. Every run is pinned (one logical CPU per
  physical core, CPU 0's core left out) at High priority, starts only on a quiet machine, and the session starts with
  one discarded run per build (and with -SelfCheck an A-vs-A check). Per window x mode and round the ratio B/A of the
  p50 ms/frame is taken; the result is the median ratio with its min..max over the rounds and a verdict that needs the
  same sign in at least 80 % of the rounds. The SH-2 and "other" stages get the same treatment (from the bench's
  averages). Writes measure.csv (every run), measure-summary.csv, measure.md, provenance.json.
.EXAMPLE
  tools\perf\measure.ps1 -A <base bin> -B <new bin> -SelfCheck
  tools\perf\measure.ps1 -A <base bin> -B <new bin> -Windows vf2,megamix -Modes unthr -Rounds 8
#>
param(
    [Parameter(Mandatory)][string]$A,
    [Parameter(Mandatory)][string]$B,
    [int]$Rounds = 6,
    [int]$Frames = 1200,
    [string]$Windows = '',
    [string]$Modes = 'thr,unthr',
    [string[]]$Extra = @(),
    [string]$Affinity = '',            # hex mask; default: one logical CPU per physical core, without CPU 0's core
    [double]$MaxLoad = 5,              # % CPU used by others before a run
    [int]$QuietWaitSeconds = 120,
    [switch]$RequireQuiet,             # fail instead of running (flagged) when the machine stays busy
    [double]$OutlierPercent = 8,       # a run this far from its cell's median p50 is re-run
    [int]$MaxReruns = 3,
    [switch]$SelfCheck,
    [string]$SelfCheckWindows = 'gh,vf2',
    [int]$SelfCheckRounds = 4,
    [double]$SelfCheckLimit = 3,       # max paired spread (max - min of B/A, %) allowed in the self-check
    [string]$Records = $(if ($env:BRIMIR_PERF_RECORDS) { $env:BRIMIR_PERF_RECORDS } else { 'C:\Users\david\projects\Brimir\docs\superpowers\records\2026-11-11-perf-recon' }),
    [string]$Roms = 'F:\OneDrive\Roms\saturn',
    [string]$BiosDir = 'F:\OneDrive\Roms\BIOS',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PerfCpu {
    [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    // Share of all logical CPUs busy over `ms` milliseconds (kernel time includes idle time).
    public static double Load(int ms) {
        long i0, k0, u0, i1, k1, u1;
        GetSystemTimes(out i0, out k0, out u0);
        System.Threading.Thread.Sleep(ms);
        GetSystemTimes(out i1, out k1, out u1);
        double total = (k1 - k0) + (u1 - u0);
        return total > 0 ? 100.0 * (1.0 - (i1 - i0) / total) : 0.0;
    }
}
'@

$windowList = @(Select-PerfWindows $Windows)
$modeList = @(Select-PerfModes $Modes)
if ($Rounds -lt 2) { throw '-Rounds must be at least 2' }
if ($Rounds -lt 5) { Write-Warning "Only $Rounds rounds: the verdict needs 5 or more to be meaningful" }
Assert-PerfInputs $Records $Roms $BiosDir ($windowList + (Select-PerfWindows $SelfCheckWindows))
$bench = @{ A = Get-PerfBench $A; B = Get-PerfBench $B }
if (-not $Out) { $Out = Join-Path $PerfScratch ("perf-measure\" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out, (Join-Path $Out 'logs') | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path

# Affinity: Windows numbers SMT siblings next to each other, so even CPUs are one per physical core.
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$logical = [int]$cpu.NumberOfLogicalProcessors
$cores = [int]$cpu.NumberOfCores
if ($Affinity) {
    $mask = [Convert]::ToInt64($Affinity, 16)
} else {
    $mask = 0L
    if ($logical -eq 2 * $cores) { for ($c = 1; $c -lt $cores; $c++) { $mask = $mask -bor (1L -shl (2 * $c)) } }
    else { for ($c = 1; $c -lt $logical; $c++) { $mask = $mask -bor (1L -shl $c) } }
}
$maskCpus = @(for ($c = 0; $c -lt 64; $c++) { if ($mask -band (1L -shl $c)) { $c } })
$power = ((powercfg /getactivescheme) -join ' ').Trim()

$prov = [ordered]@{
    A = Get-PerfProvenance $A; B = Get-PerfProvenance $B
    cpu = "$($cpu.Name.Trim()) ($cores cores, $logical logical)"
    affinity = ('0x{0:X}' -f $mask); affinityCpus = ($maskCpus -join ','); priority = 'High'
    powerScheme = $power; maxLoad = $MaxLoad; rounds = $Rounds; frames = $Frames; extra = ($Extra -join ' ')
}
$prov | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'provenance.json') -Encoding utf8
Write-Host "A: $($prov.A.commit)  bench $($prov.A.benchSha256.Substring(0, 16))  ($A)"
Write-Host "B: $($prov.B.commit)  bench $($prov.B.benchSha256.Substring(0, 16))  ($B)"
Write-Host "CPU affinity $($prov.affinity) (CPUs $($prov.affinityCpus)), High priority; $power"
Write-Host "out: $Out"

function Format-PerfArg([string]$s) {
    if ($s -match '[\s"]') { return '"' + ($s -replace '"', '\"') + '"' }
    return $s
}

# Waits for a quiet machine (other processes' CPU use <= MaxLoad %), returns the last load sample.
function Wait-PerfQuiet {
    $deadline = (Get-Date).AddSeconds($QuietWaitSeconds)
    while ($true) {
        $load = [PerfCpu]::Load(1000)
        if ($load -le $MaxLoad -or (Get-Date) -gt $deadline) { break }
    }
    if ($load -gt $MaxLoad -and $RequireQuiet) { throw ("Machine busy: {0:n1} % CPU used by other processes" -f $load) }
    return $load
}

# One pinned, High-priority bench run; returns its parsed row.
function Invoke-PerfRun([string]$Build, [string]$Exe, [string]$Window, [string]$Mode, [string]$Tag) {
    $sys = Join-Path $Out 'sys'
    $bios = New-PerfSysDir $sys $Window $Records $BiosDir
    $benchArgs = (Get-PerfBenchArgs $Window $sys $bios $Records $Roms) + @('--frames', "$Frames") + $PerfModes[$Mode] + $Extra
    $waitStart = Get-Date
    $load = Wait-PerfQuiet
    $wait = [math]::Round(((Get-Date) - $waitStart).TotalSeconds, 1)
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe, (($benchArgs | ForEach-Object { Format-PerfArg $_ }) -join ' '))
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $proc = [Diagnostics.Process]::Start($psi)
    $proc.ProcessorAffinity = [IntPtr]$mask
    $proc.PriorityClass = [Diagnostics.ProcessPriorityClass]::High
    $stdout = $proc.StandardOutput.ReadToEndAsync()
    $stderr = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $t = $stdout.Result + $stderr.Result
    Set-Content -LiteralPath (Join-Path $Out "logs\$Tag.txt") -Value $t
    $num = { param($pattern) $x = [regex]::Match($t, $pattern); if ($x.Success) { [double]$x.Groups[1].Value } else { [double]::NaN } }
    $mf = [regex]::Match($t, 'ms/frame\s+: avg ([\d.]+)\s+p50 ([\d.]+)\s+p95 ([\d.]+)')
    $ymir = & $num 'Ymir_RunFrame: ([\d.]+)'
    $master = & $num 'SH2 master\s+: ([\d.]+)'
    $slave = & $num 'SH2 slave\s+: ([\d.]+)'
    return [pscustomobject]@{
        tag = $Tag; window = $Window; mode = $Mode; build = $Build
        avg = if ($mf.Success) { [double]$mf.Groups[1].Value } else { [double]::NaN }
        p50 = if ($mf.Success) { [double]$mf.Groups[2].Value } else { [double]::NaN }
        p95 = if ($mf.Success) { [double]$mf.Groups[3].Value } else { [double]::NaN }
        ymir = $ymir; sh2 = $master + $slave; other = $ymir - $master - $slave
        load = [math]::Round($load, 1); wait = $wait; busy = $load -gt $MaxLoad; exit = $proc.ExitCode
    }
}

function Write-PerfRow($Row, [string]$Csv) {
    if (-not (Test-Path -LiteralPath $Csv)) { 'tag,round,window,mode,build,avg,p50,p95,ymir,sh2,other,load,wait,busy,exit,status' | Set-Content $Csv }
    ($Row.tag, $Row.round, $Row.window, $Row.mode, $Row.build, $Row.avg, $Row.p50, $Row.p95, $Row.ymir, $Row.sh2, $Row.other,
        $Row.load, $Row.wait, $Row.busy, $Row.exit, $Row.status) -join ',' | Add-Content $Csv
}

# Paired rounds: A then B in odd rounds, B then A in even rounds. $Exes maps A/B to the binaries.
function Invoke-PerfRounds($Exes, [string[]]$WindowSet, [string[]]$ModeSet, [int]$RoundCount, [string]$Prefix, [string]$Csv) {
    $rows = [System.Collections.Generic.List[object]]::new()
    for ($r = 1; $r -le $RoundCount; $r++) {
        foreach ($w in $WindowSet) {
            foreach ($m in $ModeSet) {
                foreach ($side in $(if ($r % 2) { 'A', 'B' } else { 'B', 'A' })) {
                    $row = Invoke-PerfRun $side $Exes[$side] $w $m "$Prefix$w-$m-$side-r$r"
                    $row | Add-Member round $r
                    $row | Add-Member status 'used'
                    $rows.Add($row)
                    Write-PerfRow $row $Csv
                    Write-Host ("{0}r{1} {2,-8} {3,-5} {4}: p50 {5,7:n3}  avg {6,7:n3} ms/frame{7}{8}" -f $Prefix, $r, $w, $m, $side, $row.p50, $row.avg,
                        $(if ($row.busy) { "  BUSY $($row.load) %" } else { '' }), $(if ($row.exit) { "  EXIT $($row.exit)" } else { '' }))
                }
            }
        }
    }
    return ,$rows
}

# Per window x mode: paired B/A ratios per round for p50, SH-2 and other.
function Get-PerfSummary($Rows, [string[]]$WindowSet, [string[]]$ModeSet) {
    foreach ($w in $WindowSet) {
        foreach ($m in $ModeSet) {
            $s = [ordered]@{ window = $w; mode = $m }
            $used = @($Rows | Where-Object { $_.window -eq $w -and $_.mode -eq $m -and $_.status -eq 'used' -and $_.exit -eq 0 })
            $roundsDone = @($used.round | Sort-Object -Unique)
            foreach ($side in 'A', 'B') {
                $s["${side}_p50"] = [math]::Round((Get-PerfMedian ([double[]]@($used | Where-Object build -eq $side | ForEach-Object p50))), 3)
            }
            foreach ($metric in 'p50', 'sh2', 'other') {
                $ratios = [double[]]@(foreach ($r in $roundsDone) {
                    $ra = $used | Where-Object { $_.round -eq $r -and $_.build -eq 'A' } | Select-Object -First 1
                    $rb = $used | Where-Object { $_.round -eq $r -and $_.build -eq 'B' } | Select-Object -First 1
                    if ($ra -and $rb -and $ra.$metric -gt 0) { $rb.$metric / $ra.$metric }
                })
                $med = Get-PerfMedian $ratios
                $change = 100 * ($med - 1)
                $sign = [math]::Sign($med - 1)
                $same = @($ratios | Where-Object { [math]::Sign($_ - 1) -eq $sign -and $sign -ne 0 }).Count
                $need = [math]::Ceiling(0.8 * $ratios.Count)
                $verdict = if ($ratios.Count -lt 2 -or [double]::IsNaN($med)) { 'n/a' }
                    elseif ($same -ge $need -and $sign -lt 0) { 'faster' }
                    elseif ($same -ge $need -and $sign -gt 0) { 'SLOWER' }
                    else { 'noise' }
                $min = if ($ratios.Count) { 100 * (($ratios | Measure-Object -Minimum).Minimum - 1) } else { [double]::NaN }
                $max = if ($ratios.Count) { 100 * (($ratios | Measure-Object -Maximum).Maximum - 1) } else { [double]::NaN }
                $s["${metric}_change"] = [math]::Round($change, 2)
                $s["${metric}_min"] = [math]::Round($min, 2)
                $s["${metric}_max"] = [math]::Round($max, 2)
                $s["${metric}_spread"] = [math]::Round($max - $min, 2)
                $s["${metric}_sameSign"] = "$same/$($ratios.Count)"
                $s["${metric}_verdict"] = $verdict
            }
            $s.keep3 = $s.p50_verdict -eq 'faster' -and $s.p50_change -le -3
            [pscustomobject]$s
        }
    }
}

$csv = Join-Path $Out 'measure.csv'
$sw = [Diagnostics.Stopwatch]::StartNew()

# One discarded run per build (caches, disc image, clocks).
foreach ($side in 'A', 'B') {
    $null = Invoke-PerfRun $side $bench[$side] $windowList[0] $modeList[0] "warmup-$side"
    Write-Host "warm-up run $side done (discarded)"
}

# Re-runs a run that failed, started on a busy machine (load above MaxLoad after QuietWaitSeconds), or lies more than
# OutlierPercent from its cell's median p50 (median of the cell's quiet runs), up to MaxReruns times each; the last try
# is used in its round. Rejected runs go to $Rejected.
function Invoke-PerfReruns($Rows, $Exes, [string[]]$WindowSet, [string[]]$ModeSet, [string]$Prefix, $Rejected) {
    foreach ($w in $WindowSet) {
        foreach ($m in $ModeSet) {
            foreach ($side in 'A', 'B') {
                $cell = @($Rows | Where-Object { $_.window -eq $w -and $_.mode -eq $m -and $_.build -eq $side -and $_.status -eq 'used' })
                $quiet = @($cell | Where-Object { -not $_.busy -and $_.exit -eq 0 })
                $med = Get-PerfMedian ([double[]]@($(if ($quiet.Count) { $quiet.p50 } else { $cell.p50 })))
                foreach ($run in $cell) {
                    $try = 0
                    $current = $run
                    while ($try -lt $MaxReruns -and ($current.exit -ne 0 -or $current.busy -or [double]::IsNaN($current.p50) -or
                            [math]::Abs($current.p50 / $med - 1) * 100 -gt $OutlierPercent)) {
                        $try++
                        $current.status = 'rejected'
                        $Rejected.Add($current)
                        $redo = Invoke-PerfRun $side $Exes[$side] $w $m "$Prefix$w-$m-$side-r$($run.round)-rerun$try"
                        $redo | Add-Member round $run.round
                        $redo | Add-Member status 'used'
                        $Rows.Add($redo)
                        Write-Host ("{0}rerun r{1} {2,-8} {3,-5} {4}: {5:n3}{6} -> {7:n3}{8} ms/frame (median {9:n3})" -f $Prefix, $run.round, $w, $m, $side,
                            $current.p50, $(if ($current.busy) { ' (busy)' } else { '' }), $redo.p50, $(if ($redo.busy) { ' (busy)' } else { '' }), $med)
                        $current = $redo
                    }
                }
            }
        }
    }
}

$rejected = [System.Collections.Generic.List[object]]::new()
$selfCheckResult = $null
if ($SelfCheck) {
    $scWindows = @(Select-PerfWindows $SelfCheckWindows)
    $scExes = @{ A = $bench.A; B = $bench.A }
    $scCsv = Join-Path $Out 'selfcheck.csv'
    $scRows = Invoke-PerfRounds $scExes $scWindows $modeList $SelfCheckRounds 'selfcheck-' $scCsv
    Invoke-PerfReruns $scRows $scExes $scWindows $modeList 'selfcheck-' $rejected
    Remove-Item -LiteralPath $scCsv
    $scRows | ForEach-Object { Write-PerfRow $_ $scCsv }
    $selfCheckResult = @(Get-PerfSummary $scRows $scWindows $modeList)
    $selfCheckResult | Export-Csv (Join-Path $Out 'selfcheck-summary.csv') -NoTypeInformation
    $selfCheckResult | Format-Table window, mode, A_p50, p50_change, p50_min, p50_max, p50_spread -AutoSize | Out-String | Write-Host
    $worst = ($selfCheckResult | Measure-Object p50_spread -Maximum).Maximum
    if ($worst -gt $SelfCheckLimit) {
        Write-Host ("SELF-CHECK FAILED: A vs A paired spread {0:n2} % > {1} %: the machine is too noisy to measure" -f $worst, $SelfCheckLimit) -ForegroundColor Red
        exit 3
    }
    Write-Host ("self-check OK: A vs A paired spread at most {0:n2} %" -f $worst) -ForegroundColor Green
}

$rows = Invoke-PerfRounds $bench $windowList $modeList $Rounds '' $csv
Invoke-PerfReruns $rows $bench $windowList $modeList '' $rejected
Remove-Item -LiteralPath $csv
$rows | ForEach-Object { Write-PerfRow $_ $csv }
$rejected | ForEach-Object { Write-PerfRow $_ (Join-Path $Out 'rejected.csv') }
Remove-Item -LiteralPath (Join-Path $Out 'sys') -Recurse -Force -ErrorAction SilentlyContinue
$sw.Stop()

$summary = @(Get-PerfSummary $rows $windowList $modeList)
$summary | Export-Csv (Join-Path $Out 'measure-summary.csv') -NoTypeInformation
$f = { param($v) if ($v -is [double] -and [double]::IsNaN($v)) { '-' } else { '{0:n2}' -f $v } }
$busy = @($rows | Where-Object { $_.status -eq 'used' -and $_.busy }).Count
$md = @("# measure: A $($prov.A.commit) vs B $($prov.B.commit)", '',
    "$Rounds paired rounds (A,B / B,A alternating), $Frames frames per window$(if ($Extra) { ", extra: $($Extra -join ' ')" }), CPUs $($prov.affinityCpus) at High priority, $power. Wall $([math]::Round($sw.Elapsed.TotalMinutes)) min; $($rejected.Count) runs rejected and re-run (failed, started above $MaxLoad % background load, or > $OutlierPercent % from the cell median; self-check included); $busy used runs still started above $MaxLoad %.", '',
    'Change = median over rounds of B/A - 1 (p50 ms/frame; SH-2 and other from the bench averages), [min..max] over the rounds. Verdict faster/SLOWER needs the same sign in >= 80 % of the rounds; "keep" = faster by >= 3 %. In thr mode "other" includes the wait for the VDP threads.', '')
if ($selfCheckResult) {
    $md += "Self-check A vs A ($SelfCheckRounds rounds): " + (($selfCheckResult | ForEach-Object { "$($_.window) $($_.mode) $(& $f $_.p50_change) % [$(& $f $_.p50_min)..$(& $f $_.p50_max)]" }) -join '; ')
    $md += ''
}
$md += @('| window | mode | A p50 | B p50 | change % [min..max] | same sign | verdict | keep | SH-2 change % [min..max] | SH-2 verdict | other change % [min..max] | other verdict |',
    '|---|---|---|---|---|---|---|---|---|---|---|---|')
$md += $summary | ForEach-Object {
    "| $($_.window) | $($_.mode) | $(& $f $_.A_p50) | $(& $f $_.B_p50) | $(& $f $_.p50_change) [$(& $f $_.p50_min)..$(& $f $_.p50_max)] | $($_.p50_sameSign) | $($_.p50_verdict) | $(if ($_.keep3) { 'yes' } else { '' }) | $(& $f $_.sh2_change) [$(& $f $_.sh2_min)..$(& $f $_.sh2_max)] | $($_.sh2_verdict) | $(& $f $_.other_change) [$(& $f $_.other_min)..$(& $f $_.other_max)] | $($_.other_verdict) |"
}
if ($rejected.Count) {
    $md += @('', ('Rejected runs: ' + (($rejected | ForEach-Object { "$($_.tag) $(& $f $_.p50)" }) -join ', ')))
}
$md | Set-Content (Join-Path $Out 'measure.md') -Encoding utf8
$md | Write-Host
Write-Host ("{0} runs in {1:n1} min; results in {2}" -f $rows.Count, $sw.Elapsed.TotalMinutes, $Out)
if (@($rows | Where-Object { $_.status -eq 'used' -and $_.exit -ne 0 }).Count) { Write-Host 'Some runs failed (see logs)' -ForegroundColor Red; exit 2 }
exit 0