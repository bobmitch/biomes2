# A/B runner for --gpu-profile: one line per variant (frame time as it runs,
# the serialised groups). Run from the repository root:
#   tools\profile\prof_ab.ps1 <label> "<flags>" [res W H] [cam x y z yaw pitch | none]
# Defaults: 3840 2160 at seed 0's densest stand (CLAUDE.md, Trees).
param([string]$label, [string]$extra = "", [string]$res = "3840 2160", [string]$cam = "-704 33.1 -448 0 0")
$shot = "build/ab_$label.bmp"
Remove-Item -ErrorAction SilentlyContinue "$shot*", build/engine_error.txt
$camarg = if ($cam -eq "none") { "" } else { "--cam $cam" }
Start-Process -Wait build\engine.exe -ArgumentList "--shot $shot --res $res $camarg --gpu-profile 120 $extra"
if (Test-Path build/engine_error.txt) { Get-Content build/engine_error.txt | Select-String -NotMatch "warning" | Select-Object -First 5 }
$g = Get-Content "$shot.gpu.txt"
$frame = ($g | Select-String "as it runs").Line -replace '.*\): ', ''
$groups = ($g | Select-String "^sum of passes").Line
$pl = ($g | Select-String "^trees lod0|^trees lod1|^trees impostors|^terrain |^foliage ").Line -join " | "
"{0,-22} frame {1}`n    {2}`n    {3}" -f $label, $frame, $groups, $pl
