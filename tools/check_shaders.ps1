# Compile chaque point d'entrée HLSL avec fxc (le même compilateur que D3DCompile utilisé à l'exécution).
# Usage : powershell -ExecutionPolicy Bypass -File tools/check_shaders.ps1
$ErrorActionPreference = "Stop"
$fxc = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe" |
       Sort-Object FullName -Descending | Select-Object -First 1
if (-not $fxc) { Write-Error "fxc.exe introuvable (installer le Windows SDK)"; exit 1 }
$dir = Join-Path $PSScriptRoot "..\shaders"
$jobs = @(
  @("ocean_spectrum.hlsl", "cs_5_1", "InitSpectrum", @()),
  @("ocean_spectrum.hlsl", "cs_5_1", "TimeSpectrum", @()),
  @("ocean_fft.hlsl",      "cs_5_1", "main", @("/DFFT_N=256", "/DFFT_LOG_N=8")),
  @("ocean_fft.hlsl",      "cs_5_1", "main", @("/DFFT_N=128", "/DFFT_LOG_N=7")),
  @("ocean_assemble.hlsl", "cs_5_1", "main", @()),
  @("ocean_draw.hlsl",     "vs_5_1", "VSMain", @()),
  @("ocean_draw.hlsl",     "ps_5_1", "PSMain", @()),
  @("cloud_noise.hlsl",    "cs_5_1", "BakeShape", @()),
  @("cloud_noise.hlsl",    "cs_5_1", "BakeDetail", @()),
  @("cloud_noise.hlsl",    "cs_5_1", "BakeWeather", @()),
  @("cloud_march.hlsl",    "cs_5_1", "main", @()),
  @("cloud_reproject.hlsl","cs_5_1", "main", @()),
  @("composite.hlsl",      "vs_5_1", "VSMain", @()),
  @("composite.hlsl",      "ps_5_1", "PSMain", @()),
  @("tonemap.hlsl",        "vs_5_1", "VSMain", @()),
  @("tonemap.hlsl",        "ps_5_1", "PSMain", @()),
  @("wind.hlsl",           "cs_5_1", "main", @()),
  @("rain.hlsl",           "cs_5_1", "Occlusion", @()),
  @("rain.hlsl",           "cs_5_1", "Simulate", @()),
  @("rain_draw.hlsl",      "vs_5_1", "VSMain", @()),
  @("rain_draw.hlsl",      "ps_5_1", "PSMain", @()),
  @("wetness.hlsl",        "cs_5_1", "main", @()),
  @("scene_draw.hlsl",     "vs_5_1", "VSMain", @()),
  @("scene_draw.hlsl",     "ps_5_1", "PSMain", @())
)
$failed = 0
foreach ($j in $jobs) {
  $file = Join-Path $dir $j[0]
  $fxcArgs = @("/nologo", "/T", $j[1], "/E", $j[2], "/O3", "/Ges", "/Fo", "NUL") + $j[3] + @($file)
  $out = & $fxc.FullName @fxcArgs 2>&1
  if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "ÉCHEC  $($j[0]) : $($j[2])"; $out | Write-Host }
  else { Write-Host "ok     $($j[0]) : $($j[2])" }
}
if ($failed -gt 0) { Write-Error "$failed shader(s) en erreur"; exit 1 }
