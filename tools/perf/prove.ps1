<#
.SYNOPSIS
  Proves two builds produce identical output on the recon windows: per-frame frame hashes (--hash) and audio hashes
  (--audio-hash, unless -NoAudio) on every window in both threading modes, plus per-frame save-state hashes (-States,
  unthreaded runs). Every frame from the state load (or boot) is checked: --warmup 0 --frames <window warmup + Frames>.
  Exit 0 identical; 1 a difference (prints the first differing window/frame); 2 a run, setup or script error.
.EXAMPLE
  tools\perf\prove.ps1 -Base $env:TEMP\opencode\perf-base-<sha7>\build-lto\bin -New .\build-lto\bin -States
#>
param(
    [Parameter(Mandatory)][string]$Base,
    [Parameter(Mandatory)][string]$New,
    [switch]$NoAudio,
    [switch]$States,
    [switch]$AllowSame,
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
$previousTz = $env:TZ
try {
    . (Join-Path $PSScriptRoot 'common.ps1')
    $Audio = -not $NoAudio
    if ($Frames -lt 1 -or $StateEvery -lt 1) { throw '-Frames and -StateEvery must be at least 1' }

    $windowList = @(Select-PerfWindows $Windows)
    $modeList = @(Select-PerfModes $Modes)
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
        if ($Audio -and $help -notmatch '--audio-hash') { throw "$exe has no --audio-hash (Task 0 or later; -NoAudio for older benches)" }
        if ($States -and $help -notmatch '--save-state-every') { throw "$exe has no --save-state-every (Task 0 or later)" }
        if ($help -notmatch '--fixed-rtc') { $fixedRtc = $false }
    }
    if (-not $fixedRtc -and $windowList -contains 'bios') { Write-Warning 'A bench has no --fixed-rtc: the bios window runs on the host clock' }
    # The core converts RTC dates with the C library's local time, whose zone rules differ between hosts and C
    # libraries (state bytes then differ by the zone offset). Every bench run here is on UTC: benches from before
    # --fixed-rtc pinned the zone itself follow TZ too (MSVC runtime and glibc), and the parallel runspaces inherit it.
    $env:TZ = 'UTC0'

    $prov = [ordered]@{ base = Get-PerfProvenance $Base; new = Get-PerfProvenance $New }
    $prov | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Out 'provenance.json') -Encoding utf8
    Write-Host "base: $($prov.base.commit)  bench $($prov.base.benchSha256.Substring(0, 16))  ($Base)"
    Write-Host "new : $($prov.new.commit)  bench $($prov.new.benchSha256.Substring(0, 16))  ($New)"
    Write-Host "out : $Out"
    if ($prov.base.benchSha256 -eq $prov.new.benchSha256) {
        if (-not $AllowSame) { throw 'SELF-COMPARE: base and new are the same brimir_bench (equal SHA-256); pass -AllowSame for a harness self-check' }
        Write-Warning 'SELF-COMPARE: base and new are the same brimir_bench'
    }

    $pairs = @(foreach ($w in $windowList) { foreach ($m in $modeList) { [pscustomobject]@{ window = $w; mode = $m } } })
    $commonPs1 = Join-Path $PSScriptRoot 'common.ps1'
    $sw = [Diagnostics.Stopwatch]::StartNew()

    $results = @($pairs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        . $using:commonPs1
        $p = $_
        $u = @{ benchBase = $using:benchBase; benchNew = $using:benchNew; out = $using:Out; records = $using:Records; roms = $using:Roms
            biosDir = $using:BiosDir; audio = $using:Audio; fixedRtc = $using:fixedRtc; states = $using:States; frames = $using:Frames
            stateEvery = $using:StateEvery }
        $tag = "$($p.window)-$($p.mode)"
        $withStates = $u.states -and $p.mode -eq 'unthr'
        $total = $PerfWindows[$p.window].warmup + $u.frames
        $t0 = Get-Date
        # One bench run; $stateArgs: --save-state-every N <dir|->.
        $runBench = {
            param($side, $frames, $stateArgs, $suffix)
            $exe = if ($side -eq 'base') { $u.benchBase } else { $u.benchNew }
            $sys = Join-Path $u.out "sys\$tag-$side$suffix"
            $bios = New-PerfSysDir $sys $p.window $u.records $u.biosDir
            $a = (Get-PerfBenchArgs $p.window $sys $bios $u.records $u.roms -NoWarmup) + @('--frames', "$frames", '--hash') + $PerfModes[$p.mode]
            if ($u.audio) { $a += '--audio-hash' }
            if ($u.fixedRtc -and -not $PerfWindows[$p.window].game) { $a += '--fixed-rtc' }
            $a += $stateArgs
            $log = Join-Path $u.out "logs\$tag-$side$suffix.txt"
            & $exe @a > $log 2> "$log.err"
            $code = $LASTEXITCODE
            Remove-Item -LiteralPath $sys -Recurse -Force -ErrorAction SilentlyContinue
            $errLine = @(Get-Content -LiteralPath "$log.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() }) | Select-Object -First 1
            return @{ exit = $code; err = $errLine
                lines = @(Get-Content -LiteralPath $log | Where-Object { $_ -match '^(frame|audio|state) ' }) }
        }
        $stateArgs = if ($withStates) { @('--save-state-every', "$($u.stateEvery)", '-') } else { @() }
        $runs = @{}
        foreach ($side in 'base', 'new') { $runs[$side] = & $runBench $side $total $stateArgs '' }

        $expect = @{ frame = $total; audio = $(if ($u.audio) { $total } else { 0 })
            state = $(if ($withStates) { [math]::Floor($total / $u.stateEvery) } else { 0 }) }
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
                        $frameNo = [int](($b[$i] -split ' ')[1])
                        $first = "$kind at frame $frameNo from load (window frame $($frameNo - $PerfWindows[$p.window].warmup)): base '$($b[$i])' new '$($n[$i])'"
                        if ($kind -eq 'state') {
                            # Write just that state on both sides (--save-state-every i+1 over i+1 frames) for brimir_state_diff.
                            # The re-run must succeed and reproduce the detected hash, else the kept state is not the one
                            # that differed.
                            foreach ($side in 'base', 'new') {
                                $dir = Join-Path $u.out "states\$tag-$side"
                                New-Item -ItemType Directory -Force $dir | Out-Null
                                $re = & $runBench $side ($frameNo + 1) @('--save-state-every', "$($frameNo + 1)", $dir) '-state'
                                $want = if ($side -eq 'base') { $b[$i] } else { $n[$i] }
                                $got = @($re.lines | Where-Object { $_.StartsWith("state $frameNo ") }) | Select-Object -First 1
                                if ($re.exit -ne 0) {
                                    Write-Warning "$tag ${side}: state re-run failed (exit $($re.exit)); the kept state may be missing"
                                    $first += " [$side state re-run exit $($re.exit)]"
                                } elseif ($got -ne $want) {
                                    Write-Warning "$tag ${side}: state re-run gave '$got', expected '$want'; the kept state is not the detected one"
                                    $first += " [$side state re-run mismatch: '$got']"
                                }
                            }
                        }
                        break
                    }
                }
                if ($result -ne 'same') { break }
            }
        }
        $errs = @(foreach ($side in 'base', 'new') { if ($runs[$side].err) { "$side stderr: $($runs[$side].err)" } })
        [pscustomobject]@{ window = $p.window; mode = $p.mode; result = $result; checked = (@($counts) + $errs) -join ', '
            seconds = [int]((Get-Date) - $t0).TotalSeconds; first = $first }
    })
    $sw.Stop()
    if ($results.Count -ne $pairs.Count) { throw "Got $($results.Count) results for $($pairs.Count) window x mode pairs" }

    $order = @{}
    $k = 0
    foreach ($p in $pairs) { $order["$($p.window)-$($p.mode)"] = $k++ }
    $results = @($results | Sort-Object { $order["$($_.window)-$($_.mode)"] })
    $results | Export-Csv (Join-Path $Out 'prove.csv') -NoTypeInformation
    $md = @("# prove: $($prov.base.commit) vs $($prov.new.commit)", '',
        "Every frame from the state load: window warmup + $Frames frames; audio $Audio; states $([bool]$States) (every $StateEvery, unthreaded); wall $([math]::Round($sw.Elapsed.TotalMinutes, 1)) min.", '',
        '| window | mode | result | checked | s | first difference |', '|---|---|---|---|---|---|')
    $md += $results | ForEach-Object { "| $($_.window) | $($_.mode) | $($_.result) | $($_.checked) | $($_.seconds) | $($_.first) |" }
    $md | Set-Content (Join-Path $Out 'prove.md') -Encoding utf8
    $results | Format-Table window, mode, result, checked, seconds, first -AutoSize | Out-String -Width 300 | Write-Host

    $bad = @($results | Where-Object result -ne 'same')
    Write-Host ("{0} pairs in {1:n1} min; results in {2}" -f $results.Count, $sw.Elapsed.TotalMinutes, $Out)
    if ($bad.Count) {
        $f = $bad[0]
        Write-Host "FIRST DIFFERENCE: $($f.window) $($f.mode): $($f.first)" -ForegroundColor Red
        if ($f.first -like 'state*') {
            Write-Host "The first differing state of each side is in $Out\states\$($f.window)-$($f.mode)-{base,new}: brimir_state_diff --strict <base> <new>"
        }
        exit $(if (@($bad | Where-Object result -eq 'ERROR').Count) { 2 } else { 1 })
    }
    Write-Host 'IDENTICAL: no difference on any window' -ForegroundColor Green
    exit 0
} catch {
    Write-Host "prove.ps1 error: $_" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace
    exit 2
} finally {
    $env:TZ = $previousTz
}
