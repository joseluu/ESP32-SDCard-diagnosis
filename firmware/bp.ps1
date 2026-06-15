# bp.ps1 — build (and optionally flash) the firmware from a clean (non-MSYS)
# environment. PlatformIO's ESP-IDF tooling refuses to run under MSYS/MinGW,
# so we drop MSYSTEM and the Git MSYS shell dirs from PATH but keep Git\cmd.
#
#   pwsh bp.ps1            # build only
#   pwsh bp.ps1 upload     # build + flash to COM7
param([string]$target = "")

Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
$env:PATH = (($env:PATH -split ';' |
    Where-Object { $_ -notmatch 'Git\\(usr|mingw64)' -and $_ -notmatch 'Git\\bin' })) -join ';'
$env:PATH = "C:\Program Files\Git\cmd;" + $env:PATH

$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$proj = "C:\Users\josel\hobby_w\ESP32\SDCard_diagnosis\firmware"

if ($target -eq "upload") {
    & $pio run -d $proj -e cyd_esp32 -t upload 2>&1 | Select-Object -Last 25
} else {
    & $pio run -d $proj -e cyd_esp32 2>&1 | Select-Object -Last 25
}
