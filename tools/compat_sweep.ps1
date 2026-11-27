<#
.SYNOPSIS
  Saturn compatibility sweep: runs brimir_bench over every top-level CHD of a folder and writes screenshots,
  per-game contact sheets, results.json and index.md.

.DESCRIPTION
  One brimir_bench process per game (so a crash or hang ends only that game), software renderer, default options,
  BIOS chosen by Auto from a scratch copy of the BIOS folder (as brimir_bios=auto does in RetroArch), the cartridge
  the game database inserts (as brimir_cartridge=auto does), --fixed-rtc so reruns are identical, an input script and
  a screenshot every -ScreenshotEvery frames. Nothing is written into -BiosDir or -GameDir; the BIOS files and the
  RTC settings files are copied into a fresh scratch system directory per game (the core rewrites the RTC files) and
  the scratch is deleted at the end.

  Output (-OutDir): <game>\f<k>.png, <game>\summary.json (the bench's --sweep-summary), <game>\run.log,
  <game>\result.json, <game>\sheet.png, results.json, index.md, sweep.log, and sweep.done when the run has ended.
  Status per game: loaded (summary written, no load error), load error, crash (the process ended without a summary,
  the exit code is recorded) or timeout (killed after -TimeoutMinutes of wall time).

.EXAMPLE
  pwsh tools\compat_sweep.ps1
  pwsh tools\compat_sweep.ps1 -Only 'Sega Rally*','Panzer*' -OutDir C:\Temp\rerun
  pwsh tools\compat_sweep.ps1 -SheetsOnly                      # rebuild sheets, results.json and index.md only
  pwsh tools\compat_sweep.ps1 -CompareWith C:\Temp\first       # compare -OutDir's games with another sweep's
#>
[CmdletBinding()]
param(
    [string]$GameDir = 'F:\OneDrive\Roms\saturn',
    [string]$BiosDir = 'F:\OneDrive\Roms\BIOS',
    [string]$OutDir,
    [int]$Frames = 10800,
    [string]$InputScript = 'start:120/600,a:30/900',
    [int]$ScreenshotEvery = 600,
    [double]$TimeoutMinutes = 15,
    [int]$LogLimitKB = 1024,
    [string]$Bench,
    [string]$Scratch = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\compat-sweep'),
    [string[]]$Only,
    [switch]$SheetsOnly,
    [string]$CompareWith,
    [switch]$KeepScratch
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'docs\superpowers\records\2026-11-27-compat-sweep' }
if (-not $Bench) { $Bench = Join-Path $repo 'build\bin\brimir_bench.exe' }

# The files Brimir accepts as Saturn BIOS (src/bridge/bios_select.cpp), the two ROM cartridge dumps the core looks
# for in the system folder, and the configured RTC settings (BIOS language and clock; one per BIOS region)
$SystemFiles = @(
    'Sega Saturn BIOS v1.01 (JAP).bin', 'Sega Saturn BIOS v1.00 (JAP).bin', 'sega_101.bin', 'mpr-17933.bin',
    'sega_100.bin', 'Sega Saturn BIOS (EUR).bin',
    'mpr-18811-mx.ic1', 'mpr-19367-mx.ic1',
    'brimir_saturn_rtc_jp.smpc', 'brimir_saturn_rtc_us_eu.smpc'
)

function Get-Slug([string]$name) {
    $s = ($name -replace '[^A-Za-z0-9._-]+', '_').Trim('_')
    if (-not $s) { $s = 'game' }
    return $s
}

function Write-Log([string]$msg) {
    $line = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $msg
    Write-Host $line
    if ($script:LogFile) { Add-Content -LiteralPath $script:LogFile -Value $line -Encoding utf8 }
}

function Read-JsonFile([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try { return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { return $null }
}

function Get-Screenshots([string]$dir) {
    $items = foreach ($f in Get-ChildItem -LiteralPath $dir -Filter 'f*.png') {
        if ($f.BaseName -match '^f(\d+)$') {
            [pscustomobject]@{
                frame  = [int]$Matches[1]
                file   = $f.Name
                sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
            }
        }
    }
    return @($items | Sort-Object frame)
}

# A game that floods the core's debug log (one reached 13 GB in 15 minutes) keeps only the first $LogLimitKB
function Limit-LogSize([string]$path) {
    $max = $LogLimitKB * 1KB
    $len = (Get-Item -LiteralPath $path).Length
    if ($len -le $max) { return }
    $buf = New-Object byte[] $max
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { $null = $fs.Read($buf, 0, $max) } finally { $fs.Dispose() }
    $note = [System.Text.Encoding]::ASCII.GetBytes("`n[compat_sweep: log truncated, $len bytes written, first $max kept]`n")
    $fs = [System.IO.File]::Open($path, 'Create', 'Write')
    try { $fs.Write($buf, 0, $max); $fs.Write($note, 0, $note.Length) } finally { $fs.Dispose() }
}

function Invoke-Game([System.IO.FileInfo]$disc, [string]$gameOut) {
    $summaryPath = Join-Path $gameOut 'summary.json'
    $logPath = Join-Path $gameOut 'run.log'
    $errPath = Join-Path $gameOut 'run.err'

    $work = Join-Path $Scratch 'work'
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $Scratch 'system') -Destination $work -Recurse

    $benchArgs = @('--game', "`"$($disc.FullName)`"", '--system-dir', "`"$work`"", '--frames', $Frames,
        '--screenshot-every', $ScreenshotEvery, "`"$gameOut`"", '--input-script', $InputScript,
        '--sweep-summary', "`"$summaryPath`"", '--fixed-rtc')

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Bench -ArgumentList $benchArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput $logPath -RedirectStandardError $errPath
    $null = $p.Handle   # keeps the exit code available after exit
    $timedOut = -not $p.WaitForExit($TimeoutMinutes * 60 * 1000)
    if ($timedOut) {
        & taskkill.exe /PID $p.Id /T /F *> $null
        $null = $p.WaitForExit(30000)
    }
    $sw.Stop()
    $exit = if ($timedOut) { $null } else { $p.ExitCode }
    Limit-LogSize $logPath
    Limit-LogSize $errPath
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue

    $summary = Read-JsonFile $summaryPath
    if ($timedOut) {
        $status = 'timeout'
    } elseif ($summary -and $summary.load_error) {
        $status = 'load error'
    } elseif ($summary -and $exit -eq 0) {
        $status = 'loaded'
    } elseif ($summary) {
        $status = "error (exit $exit)"
    } else {
        $status = 'crash'
    }
    return [ordered]@{ status = $status; exit_code = $exit; driver_wall_seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
}

function New-ContactSheet([string]$dir, [string]$title, [string]$outPng) {
    Add-Type -AssemblyName System.Drawing
    $shots = Get-Screenshots $dir
    if ($shots.Count -eq 0) { return $false }

    $cols = 6; $cellW = 256; $cellH = 192; $labelH = 20; $pad = 6; $headH = 28
    $rows = [math]::Ceiling($shots.Count / $cols)
    $cols = [math]::Min($cols, $shots.Count)
    $w = $pad + $cols * ($cellW + $pad)
    $h = $headH + $pad + $rows * ($cellH + $labelH + $pad)

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $font = New-Object System.Drawing.Font 'Consolas', 11
    $big = New-Object System.Drawing.Font 'Consolas', 13, ([System.Drawing.FontStyle]::Bold)
    $white = [System.Drawing.Brushes]::White
    try {
        $g.Clear([System.Drawing.Color]::FromArgb(24, 24, 24))
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawString($title, $big, $white, $pad, 5)
        for ($i = 0; $i -lt $shots.Count; $i++) {
            $x = $pad + ($i % $cols) * ($cellW + $pad)
            $y = $headH + $pad + [math]::Floor($i / $cols) * ($cellH + $labelH + $pad)
            $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y, $cellW, $cellH)
            $bytes = [System.IO.File]::ReadAllBytes((Join-Path $dir $shots[$i].file))
            $ms = New-Object System.IO.MemoryStream (, $bytes)
            $img = [System.Drawing.Image]::FromStream($ms)
            try {
                $scale = [math]::Min($cellW / $img.Width, $cellH / $img.Height)
                $dw = [int]($img.Width * $scale); $dh = [int]($img.Height * $scale)
                $g.DrawImage($img, $x + [int](($cellW - $dw) / 2), $y + [int](($cellH - $dh) / 2), $dw, $dh)
            } finally { $img.Dispose(); $ms.Dispose() }
            $g.DrawString(('frame {0}' -f $shots[$i].frame), $font, $white, $x, $y + $cellH + 2)
        }
    } finally { $g.Dispose() }
    $bmp.Save($outPng, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose(); $font.Dispose(); $big.Dispose()
    return $true
}

function Build-Reports {
    $results = @()
    foreach ($d in Get-ChildItem -LiteralPath $OutDir -Directory | Sort-Object Name) {
        $r = Read-JsonFile (Join-Path $d.FullName 'result.json')
        if (-not $r) { continue }
        # The sheet is rebuilt from the screenshots on disk each time (also for -SheetsOnly)
        $sheet = Join-Path $d.FullName 'sheet.png'
        if (New-ContactSheet $d.FullName $r.name $sheet) { $r.sheet = "$($d.Name)/sheet.png" } else { $r.sheet = $null }
        $results += $r
    }
    $results = @($results | Sort-Object { $_.name.ToLowerInvariant() })
    $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutDir 'results.json') -Encoding utf8

    $counts = [ordered]@{ loaded = 0; crash = 0; timeout = 0; 'load error' = 0; other = 0 }
    foreach ($r in $results) {
        if ($counts.Contains($r.status)) { $counts[$r.status]++ } else { $counts['other']++ }
    }

    $md = New-Object System.Collections.Generic.List[string]
    $md.Add('# Saturn compatibility sweep')
    $md.Add('')
    $md.Add(('{0} games. loaded {1}, crash {2}, timeout {3}, load error {4}{5}.' -f $results.Count, $counts['loaded'],
            $counts['crash'], $counts['timeout'], $counts['load error'],
            $(if ($counts['other']) { ", other $($counts['other'])" } else { '' })))
    $first = $results | Select-Object -First 1
    if ($first -and $first.params) {
        $md.Add('')
        $runLine = ('Run: {0} frames, input `{1}`, screenshot every {2} frames, `--fixed-rtc`, BIOS Auto, software renderer, ' +
            'cartridge from the game database.') -f $first.params.frames, $first.params.input_script, $first.params.screenshot_every
        $md.Add($runLine)
    }
    $md.Add('')
    $md.Add('| # | Game | Status | Longest identical-frame run | Audio peak | Longest silence | BIOS | Cartridge | Sheet |')
    $md.Add('|---|------|--------|-----------------------------|------------|-----------------|------|-----------|-------|')
    $n = 0
    foreach ($r in $results) {
        $n++
        $s = $r.summary
        $status = $r.status
        if ($status -eq 'crash') { $status = 'crash (exit {0})' -f $r.exit_code }
        if ($status -eq 'load error' -and $s) { $status = 'load error: ' + ($s.load_error -replace '\|', '/') }
        $ident = '-'; $peak = '-'; $sil = '-'; $bios = '-'; $cart = '-'
        if ($s -and -not $s.load_error) {
            $ident = '{0} frames from {1}' -f $s.identical_frames.length, $s.identical_frames.start
            $peak = $s.audio.peak
            $sil = '{0} frames from {1}' -f $s.audio.longest_silent_frames, $s.audio.longest_silent_start
            $bios = $s.bios
            $cart = $s.cartridge
        }
        $link = if ($r.sheet) { "[sheet]($($r.sheet -replace ' ', '%20'))" } else { '-' }
        $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f $n, ($r.name -replace '\|', '/'),
                $status, $ident, $peak, $sil, $bios, $cart, $link))
    }
    Set-Content -LiteralPath (Join-Path $OutDir 'index.md') -Value ($md -join "`n") -Encoding utf8 -NoNewline
    return $counts
}

function Compare-Sweeps([string]$other) {
    $a = Read-JsonFile (Join-Path $OutDir 'results.json')
    $b = Read-JsonFile (Join-Path $other 'results.json')
    if (-not $a -or -not $b) { throw "results.json missing in $OutDir or $other" }
    $bByFile = @{}; foreach ($r in $b) { $bByFile[$r.file] = $r }
    $same = 0; $diff = 0
    foreach ($r in $a) {
        $o = $bByFile[$r.file]
        if (-not $o) { continue }
        $key = {
            param($x)
            $s = $x.summary
            $parts = @($x.status)
            if ($s) { $parts += ($s | Select-Object frames_run, identical_frames, audio, master_pc, slave_pc, bios, cartridge | ConvertTo-Json -Compress -Depth 5) }
            $parts += ($x.screenshots | ForEach-Object { "$($_.frame):$($_.sha256)" })
            return ($parts -join '|')
        }
        if ((& $key $r) -eq (& $key $o)) {
            $same++
            Write-Host ("identical  {0}  ({1} screenshots)" -f $r.file, @($r.screenshots).Count)
        } else {
            $diff++
            Write-Host ("DIFFERENT  {0}" -f $r.file)
        }
    }
    Write-Host "compared: $same identical, $diff different"
    return ($diff -eq 0 -and $same -gt 0)
}

# ---------------------------------------------------------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$script:LogFile = Join-Path $OutDir 'sweep.log'

if ($CompareWith) {
    $ok = Compare-Sweeps $CompareWith
    exit $(if ($ok) { 0 } else { 1 })
}

if ($SheetsOnly) {
    $c = Build-Reports
    Write-Log ("reports rebuilt: " + (($c.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', '))
    return
}

if (-not (Test-Path -LiteralPath $Bench)) { throw "brimir_bench not found: $Bench (build target brimir_bench)" }
$doneMarker = Join-Path $OutDir 'sweep.done'
Remove-Item -LiteralPath $doneMarker -ErrorAction SilentlyContinue

$discs = @(Get-ChildItem -LiteralPath $GameDir -Filter '*.chd' -File | Sort-Object Name)
if ($Only) {
    $discs = @($discs | Where-Object {
            $base = $_.BaseName
            @($Only | Where-Object { $base -like $_ }).Count -gt 0
        })
}
if ($discs.Count -eq 0) { throw "no CHD games selected in $GameDir" }

# Scratch system directory: copies only (BiosDir stays untouched)
$template = Join-Path $Scratch 'system'
if (Test-Path -LiteralPath $Scratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force -Path $template | Out-Null
$copied = @()
foreach ($f in $SystemFiles) {
    $src = Join-Path $BiosDir $f
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $template; $copied += $f }
    else { Write-Log "not in BIOS folder, skipped: $f" }
}
Write-Log ("scratch system dir {0}: {1}" -f $template, ($copied -join ', '))
Write-Log ("sweep: {0} games, {1} frames, input '{2}', screenshots every {3}, timeout {4} min, bench {5}" -f $discs.Count,
    $Frames, $InputScript, $ScreenshotEvery, $TimeoutMinutes, $Bench)

$total = [System.Diagnostics.Stopwatch]::StartNew()
$i = 0
foreach ($disc in $discs) {
    $i++
    $slug = Get-Slug $disc.BaseName
    $gameOut = Join-Path $OutDir $slug
    if (Test-Path -LiteralPath $gameOut) { Remove-Item -LiteralPath $gameOut -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $gameOut | Out-Null

    Write-Log ("[{0}/{1}] {2}" -f $i, $discs.Count, $disc.Name)
    try {
        $run = Invoke-Game $disc $gameOut
    } catch {
        $run = [ordered]@{ status = 'crash'; exit_code = $null; driver_wall_seconds = 0; driver_error = $_.Exception.Message }
    }
    $summary = Read-JsonFile (Join-Path $gameOut 'summary.json')
    $result = [ordered]@{
        name        = $disc.BaseName
        file        = $disc.Name
        slug        = $slug
        status      = $run.status
        exit_code   = $run.exit_code
        driver_wall_seconds = $run.driver_wall_seconds
        params      = [ordered]@{ frames = $Frames; input_script = $InputScript; screenshot_every = $ScreenshotEvery; fixed_rtc = $true }
        summary     = $summary
        screenshots = @(Get-Screenshots $gameOut)
        sheet       = $null
    }
    if ($run.Contains('driver_error')) { $result.driver_error = $run.driver_error }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $gameOut 'result.json') -Encoding utf8
    $extra = if ($summary -and -not $summary.load_error) {
        ' bios {0}, cartridge {1}, {2} screenshots' -f $summary.bios, $summary.cartridge, @($result.screenshots).Count
    } elseif ($summary) { ' load_error: ' + $summary.load_error } else { '' }
    $crashNote = if ($run.status -eq 'crash') { " (exit $($run.exit_code))" } else { '' }
    Write-Log ("    -> {0}{1}{2}  ({3} s)" -f $run.status, $crashNote, $extra, $run.driver_wall_seconds)
}
$total.Stop()

$counts = Build-Reports
if (-not $KeepScratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue }
$line = "done: {0} games in {1:N1} min; {2}" -f $discs.Count, $total.Elapsed.TotalMinutes,
    (($counts.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ')
Write-Log $line
Set-Content -LiteralPath $doneMarker -Value $line -Encoding utf8
