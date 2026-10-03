param(
    [Parameter(Mandatory)] [string]$Core,
    [Parameter(Mandatory)] [string]$Tag,
    [int]$PlaySeconds = 60,
    [int]$Resets = 0
)
$ErrorActionPreference = "Stop"
$ra   = "E:\SteamLibrary\steamapps\common\RetroArch"
$h    = "C:\Users\david\AppData\Local\Temp\opencode\unload_harness"
$box  = "$h\ra_$Tag"
$game = "F:\OneDrive\Roms\saturn\Panzer Dragoon II Zwei (USA).chd"
Remove-Item -Recurse -Force $box -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$box\saves", "$box\states", "$box\sys" | Out-Null
Copy-Item "$h\sys\mpr-17933.bin" "$box\sys\"
# Copy the user's saved RTC so the BIOS doesn't stop at the clock setup screen.
Copy-Item "F:\OneDrive\Roms\BIOS\brimir_saturn_rtc_us_eu.smpc" "$box\sys\"

# Sandbox: no writes to the user's saves/BIOS dir/history/config.
@"
network_cmd_enable = "true"
network_cmd_port = "55355"
config_save_on_exit = "false"
pause_nonactive = "false"
history_list_enable = "false"
savefile_directory = "$($box -replace '\\','\\')\\saves"
savestate_directory = "$($box -replace '\\','\\')\\states"
system_directory = "$($box -replace '\\','\\')\\sys"
log_verbosity = "true"
libretro_log_level = "1"
"@ | Set-Content -Encoding ascii "$box\append.cfg"

$log = "$box\retroarch.log"
$p = Start-Process -FilePath "$ra\retroarch.exe" -WorkingDirectory $ra -PassThru `
    -ArgumentList "-L", "`"$Core`"", "`"$game`"", "--verbose", "--log-file", "`"$log`"", "--appendconfig", "`"$box\append.cfg`""

function Send-RA([string]$cmd) {
    foreach ($addr in "127.0.0.1", "::1") {
        try {
            $ip = [System.Net.IPAddress]::Parse($addr)
            $u = New-Object System.Net.Sockets.UdpClient($ip.AddressFamily)
            $b = [Text.Encoding]::ASCII.GetBytes($cmd)
            [void]$u.Send($b, $b.Length, (New-Object System.Net.IPEndPoint($ip, 55355)))
            $u.Close()
        } catch { }
    }
}
function Query-RA([string]$cmd) {
    foreach ($addr in "127.0.0.1", "::1") {
        try {
            $ip = [System.Net.IPAddress]::Parse($addr)
            $u = New-Object System.Net.Sockets.UdpClient($ip.AddressFamily)
            $u.Client.ReceiveTimeout = 2000
            $ep = New-Object System.Net.IPEndPoint($ip, 55355)
            $b = [Text.Encoding]::ASCII.GetBytes($cmd)
            [void]$u.Send($b, $b.Length, $ep)
            $any = New-Object System.Net.IPEndPoint($ip, 0)
            $r = $u.Receive([ref]$any)
            $u.Close()
            return "$addr -> " + [Text.Encoding]::ASCII.GetString($r).Trim()
        } catch { }
    }
    return "no reply"
}

# NOTE: GET_STATUS crashes this RetroArch build (access violation in retroarch.exe), so no queries.
Start-Sleep -Seconds $PlaySeconds
for ($i = 0; $i -lt $Resets; $i++) { Send-RA "RESET"; Start-Sleep -Seconds 8 }
if ($p.HasExited) { "[$Tag] RetroArch exited BEFORE Close Content (code $($p.ExitCode))"; exit 1 }

"[$Tag] sending CLOSE_CONTENT"
Send-RA "CLOSE_CONTENT"
Start-Sleep -Seconds 8
$alive = -not $p.HasExited
"[$Tag] RetroArch alive after Close Content: $alive"
if ($alive) {
    Send-RA "QUIT"; Start-Sleep -Seconds 1; Send-RA "QUIT"
    if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; "[$Tag] had to force-kill after QUIT" }
} else {
    "[$Tag] exit code: 0x{0:X}" -f $p.ExitCode
}
"---- log (relevant) ----"
Select-String -LiteralPath $log -Pattern "Brimir\]|Unloading|Content ran|dummy|SRAM|NetCMD|Exception" | Select-Object -Last 14 | ForEach-Object Line
