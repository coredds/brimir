<#
.SYNOPSIS
  Proves two builds produce identical output on the recon windows: per-frame frame hashes (--hash) on every window in
  both threading modes, plus per-frame audio hashes (-Audio) and per-frame save-state bytes (-States, unthreaded runs).
  Exits 1 and prints the first differing window/frame on any difference (2 on a run error).
.EXAMPLE
  tools\perf\prove.ps1 -Base $env:TEMP\opencode\perf-base-f395c6f\build-lto\bin -New .\build-lto\bin -Audio -States
#>
param(
    [Parameter(Mandatory)][string]$Base,
    [Parameter(Mandatory)][string]$New,
    [switch]$Audio,
    [switch]$States,
    [int]$Frames = 600,
    [int]$StateEvery = 1,
    [string]$Windows = '',
    [string]$Modes = 'thr,unthr',
    [int]$Jobs = 6,
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
$benchBase = Get-PerfBench $Base
$benchNew = Get-PerfBench $New
if (-not $Out) { $Out = Join-Path $PerfScratch ("perf-prove\" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out, (Join-Path $Out 'logs') | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path

# The bios window boots without a state: --fixed-rtc (Task 0 benches) pins its RTC, else the host clock leaks into
# work RAM and the save state. Used only when both benches have it, so both sides run the same content.
$fixedRtc = $true
foreach ($exe in @($benchBase, $benchNew)) {
    $help = (& $exe --help) -join "`n"
    if ($Audio -and $help -notmatch '--audio-hash') { throw "$exe has no --audio-hash (build it from Task 0 or later)" }
    if ($States -and $help -notmatch '--save-state-every') { throw "$exe has no --save-state-every (build it from Task 0 or later)" }
    if ($help -notmatch '--fixed-rtc') { $fixedRtc = $false }
}
if (-not $fixedRtc -and $windowList -contains 'bios') { Write-Warning 'A bench has no --fixed-rtc: the bios window runs on the host clock' }

$prov = [ordered]@{ base = Get-PerfProvenance $Base; new = Get-PerfProvenance $New }
$prov | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'provenance.json') -Encoding utf8
Write-Host "base: $($prov.base.commit)  bench $($prov.base.benchSha256.Substring(0, 16))  ($Base)"
Write-Host "new : $($prov.new.commit)  bench $($prov.new.benchSha256.Substring(0, 16))  ($New)"
Write-Host "out : $Out"

$pairs = foreach ($w in $windowList) { foreach ($m in $modeList) { [pscustomobject]@{ window = $w; mode = $m } } }
# State runs write Frames / StateEvery files of ~7 MB per side: fewer pairs at once.
$throttle = if ($States) { [math]::Min($Jobs, 3) } else { $Jobs }
$commonPs1 = Join-Path $PSScriptRoot 'common.ps1'
$sw = [Diagnostics.Stopwatch]::StartNew()

$results = $pairs | ForEach-Object -ThrottleLimit $throttle -Parallel {
    . $using:commonPs1
    $p = $_
    $tag = "$($p.window)-$($p.mode)"
    $withStates = $using:States -and $p.mode -eq 'unthr'
    $t0 = Get-Date
    $runs = @{}
    foreach ($side in 'base', 'new') {
        $exe = if ($side -eq 'base') { $using:benchBase } else { $using:benchNew }
        $sys = Join-Path $using:Out "sys\$tag-$side"
        $bios = New-PerfSysDir $sys $p.window $using:Records $using:BiosDir
        $a = Get-PerfBenchArgs $p.window $sys $bios $using:Records $using:Roms
        $a += @('--frames', "$($using:Frames)", '--hash') + $PerfModes[$p.mode]
        if ($using:Audio) { $a += '--audio-hash' }
        if ($using:fixedRtc -and -not $PerfWindows[$p.window].game) { $a += '--fixed-rtc' }
        $stDir = Join-Path $using:Out "states\$tag-$side"
        if ($withStates) {
            New-Item -ItemType Directory -Force $stDir | Out-Null
            $a += @('--save-state-every', "$($using:StateEvery)", $stDir)
        }
        $log = Join-Path $using:Out "logs\$tag-$side.txt"
        & $exe @a > $log 2> "$log.err"
        $runs[$side] = @{ exit = $LASTEXITCODE; log = $log; states = $stDir
            lines = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^(frame|audio|state) ' }) }
        Remove-Item -LiteralPath $sys -Recurse -Force -ErrorAction SilentlyContinue
    }
    $expect = @{ frame = $using:Frames; audio = $(if ($using:Audio) { $using:Frames } else { 0 })
        state = $(if ($withStates) { [math]::Floor($using:Frames / $using:StateEvery) } else { 0 }) }
    $result = 'same'
    $first = ''
    $counts = @()
    if ($runs.base.exit -ne 0 -or $runs.new.exit -ne 0) {
        $result = 'ERROR'
        $first = "exit codes base $($runs.base.exit) new $($runs.new.exit) (logs\$tag-*.txt.err)"
    } else {
        foreach ($kind in 'frame', 'audio', 'state') {
            $b = @($runs.base.lines | Where-Object { $_.StartsWith("$kind ") })
            $n = @($runs.new.lines | Where-Object { $_.StartsWith("$kind ") })
            $counts += "$kind $($n.Count)"
            if ($b.Count -ne $expect[$kind] -or $n.Count -ne $expect[$kind]) {
                $result = 'ERROR'
                $first = "$kind lines: base $($b.Count) new $($n.Count), expected $($expect[$kind])"
                break
            }
            for ($i = 0; $i -lt $b.Count; $i++) {
                if ($b[$i] -ne $n[$i]) {
                    $result = 'DIFF'
                    $first = "$kind at window frame $(($b[$i] -split ' ')[1]): base '$($b[$i])' new '$($n[$i])'"
                    break
                }
            }
            if ($result -ne 'same') { break }
        }
    }
    # Keep only the first differing state of each side (600 states are ~4 GB per side).
    $keep = if ($result -eq 'DIFF' -and $first -like 'state*') { 'frame_' + ($first -split ' ')[4].TrimEnd(':') + '.state' } else { '' }
    foreach ($side in 'base', 'new') {
        if (-not (Test-Path -LiteralPath $runs[$side].states)) { continue }
        if ($keep) {
            Get-ChildItem -LiteralPath $runs[$side].states -File | Where-Object Name -ne $keep | Remove-Item -Force
        } elseif ($result -eq 'same') {
            Remove-Item -LiteralPath $runs[$side].states -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    [pscustomobject]@{ window = $p.window; mode = $p.mode; result = $result; checked = ($counts -join ', ')
        seconds = [int]((Get-Date) - $t0).TotalSeconds; first = $first }
}
$sw.Stop()

$order = @{}
$k = 0
foreach ($p in $pairs) { $order["$($p.window)-$($p.mode)"] = $k++ }
$results = @($results | Sort-Object { $order["$($_.window)-$($_.mode)"] })
$results | Export-Csv (Join-Path $Out 'prove.csv') -NoTypeInformation
$md = @("# prove: $($prov.base.commit) vs $($prov.new.commit)", '',
    "Frames $Frames per window; audio $([bool]$Audio); states $([bool]$States) (every $StateEvery, unthreaded); wall $([int]$sw.Elapsed.TotalMinutes) min.", '',
    '| window | mode | result | checked | s | first difference |', '|---|---|---|---|---|---|')
$md += $results | ForEach-Object { "| $($_.window) | $($_.mode) | $($_.result) | $($_.checked) | $($_.seconds) | $($_.first) |" }
$md | Set-Content (Join-Path $Out 'prove.md') -Encoding utf8
$results | Format-Table window, mode, result, checked, seconds, first -AutoSize | Out-String -Width 250 | Write-Host

$bad = @($results | Where-Object result -ne 'same')
Write-Host ("{0} pairs in {1:n1} min; results in {2}" -f $results.Count, $sw.Elapsed.TotalMinutes, $Out)
if ($bad.Count) {
    $f = $bad[0]
    Write-Host "FIRST DIFFERENCE: $($f.window) $($f.mode): $($f.first)" -ForegroundColor Red
    if ($f.first -like 'state*') {
        Write-Host "The first differing state is kept in $Out\states\$($f.window)-$($f.mode)-{base,new}: brimir_state_diff --strict <base> <new>"
    }
    exit $(if (@($bad | Where-Object result -eq 'ERROR').Count) { 2 } else { 1 })
}
Write-Host 'IDENTICAL: no difference on any window' -ForegroundColor Green
exit 0
