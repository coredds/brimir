# Shared by baseline.ps1, prove.ps1 and measure.ps1 (dot-sourced): the recon windows, scratch system directories and
# build provenance. See tools/perf/README.md.

# Decimal points in CSV and markdown whatever the host locale.
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture

$PerfRepo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$PerfScratch = Join-Path $env:TEMP 'opencode'
$BiosFiles = @{ us = 'mpr-17933.bin'; jp = 'Sega Saturn BIOS v1.01 (JAP).bin' }

# The recon windows (records report.md "Content and windows"): BIOS region, disc image, recon state, warmup frames.
# bios runs the US BIOS menu with no disc; every other window loads states\<name>.state and the RTC file in sys\<name>.
$PerfWindows = [ordered]@{
    bios    = @{ region = 'us'; game = ''; warmup = 300 }
    rally   = @{ region = 'us'; game = 'Sega Rally Championship (USA).chd'; warmup = 1000 }
    vf2     = @{ region = 'jp'; game = 'Virtua Fighter 2 (Japan) (Rev B).chd'; warmup = 120 }
    burning = @{ region = 'us'; game = 'Burning Rangers (USA).chd'; warmup = 120 }
    pdz     = @{ region = 'us'; game = 'Panzer Dragoon II Zwei (USA).chd'; warmup = 120 }
    megamix = @{ region = 'us'; game = 'Fighters Megamix (USA).chd'; warmup = 120 }
    gh      = @{ region = 'us'; game = 'Guardian Heroes (USA).chd'; warmup = 120 }
    sfz3    = @{ region = 'jp'; game = 'Street Fighter Zero 3 (Japan).chd'; warmup = 120 }
    astal   = @{ region = 'us'; game = 'Astal (USA) (1S).chd'; warmup = 120 }
}

# threaded = the core's default; unthr = --no-threaded-vdp.
$PerfModes = [ordered]@{ thr = @(); unthr = @('--no-threaded-vdp') }

function Select-PerfWindows([string]$List) {
    if (-not $List) { return @($PerfWindows.Keys) }
    $names = $List.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    foreach ($n in $names) {
        if (-not $PerfWindows.Contains($n)) { throw "Unknown window '$n' (known: $($PerfWindows.Keys -join ', '))" }
    }
    return @($names)
}

function Select-PerfModes([string]$List) {
    $names = $List.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    foreach ($n in $names) {
        if (-not $PerfModes.Contains($n)) { throw "Unknown mode '$n' (known: thr, unthr)" }
    }
    return @($names)
}

function Assert-PerfInputs([string]$Records, [string]$Roms, [string]$BiosDir, [string[]]$Windows) {
    foreach ($r in $BiosFiles.Values) {
        if (-not (Test-Path -LiteralPath (Join-Path $BiosDir $r))) { throw "Missing BIOS image: $(Join-Path $BiosDir $r)" }
    }
    foreach ($n in $Windows) {
        $w = $PerfWindows[$n]
        if (-not $w.game) { continue }
        foreach ($p in @((Join-Path $Roms $w.game), (Join-Path $Records "states\$n.state"), (Join-Path $Records "sys\$n"))) {
            if (-not (Test-Path -LiteralPath $p)) { throw "Missing input for window ${n}: $p" }
        }
    }
}

# A fresh scratch system directory for one run: the window's BIOS image (copied, the original is never opened for
# writing) and its RTC file(s). Fresh per run because the core may write the RTC file back on exit.
function New-PerfSysDir([string]$Dir, [string]$Window, [string]$Records, [string]$BiosDir) {
    if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $w = $PerfWindows[$Window]
    $bios = Join-Path $Dir $BiosFiles[$w.region]
    Copy-Item -LiteralPath (Join-Path $BiosDir $BiosFiles[$w.region]) -Destination $bios
    if ($w.game) {
        Get-ChildItem -LiteralPath (Join-Path $Records "sys\$Window") -File | Copy-Item -Destination $Dir
    }
    return $bios
}

# brimir_bench arguments for a window (without --frames and mode options). -NoWarmup: --warmup 0 (prove.ps1 hashes
# every frame from the state load or boot; it adds the window's warmup to --frames).
function Get-PerfBenchArgs([string]$Window, [string]$SysDir, [string]$Bios, [string]$Records, [string]$Roms, [switch]$NoWarmup) {
    $w = $PerfWindows[$Window]
    $a = @('--bios', $Bios)
    if ($w.game) {
        $a += @('--game', (Join-Path $Roms $w.game), '--system-dir', $SysDir, '--state', (Join-Path $Records "states\$Window.state"))
    }
    $a += @('--warmup', $(if ($NoWarmup) { '0' } else { "$($w.warmup)" }))
    return $a
}

function Get-PerfBench([string]$BinDir) {
    $exe = Join-Path $BinDir 'brimir_bench.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "No brimir_bench.exe in $BinDir" }
    return (Resolve-Path -LiteralPath $exe).Path
}

# Provenance of a bin directory: the provenance.json written by baseline.ps1 when present, plus the bench's SHA-256.
function Get-PerfProvenance([string]$BinDir) {
    $exe = Get-PerfBench $BinDir
    $info = [ordered]@{ binDir = (Resolve-Path -LiteralPath $BinDir).Path; benchSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash }
    $prov = Join-Path $BinDir 'provenance.json'
    if (Test-Path -LiteralPath $prov) {
        $p = Get-Content -LiteralPath $prov -Raw | ConvertFrom-Json
        $info.commit = $p.commit
        $info.dirty = [bool]$p.status
        $info.config = $p.cmake
        if ($p.files.'brimir_bench.exe' -and $p.files.'brimir_bench.exe' -ne $info.benchSha256) {
            $info.warning = 'brimir_bench.exe changed since provenance.json was written'
        }
    } else {
        $info.commit = '(no provenance.json)'
    }
    return [pscustomobject]$info
}

function Get-PerfMedian([double[]]$Values) {
    $s = @($Values | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
    if ($s.Count -eq 0) { return [double]::NaN }
    if ($s.Count % 2) { return $s[[int][math]::Floor($s.Count / 2)] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}
