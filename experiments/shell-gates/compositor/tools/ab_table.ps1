# ab_table.ps1 - turn runs\<Out>\summary.csv + dll_stats.csv into the README table.
# Host frame time: per mode, the mean over runs of each run's mean / p99 (runs weighted equally),
# plus the spread (min-max) of the per-run means. DLL costs: mean of the per-second values.
param([string]$Out = 'ab')
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "runs\$Out"
$s = Import-Csv "$dir\summary.csv" -Header label, frames, mean, p50, p99, max, fps
$d = Import-Csv "$dir\dll_stats.csv"
$order = 'nodll', 'off', 'passthrough', 'composite_clear', 'composite_present'
"| mode | runs | frames | host frame time mean (ms) | spread of run means | p99 (ms) | fps | DLL CPU in insertion mean / p99 (ms) | AcquireSync CPU mean (ms) | GPU draw-only mean (ms) | GPU copy+draw mean (ms) |"
"|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($m in $order) {
  $r = @($s | Where-Object label -eq $m)
  if (-not $r) { continue }
  $means = $r | ForEach-Object { [double]$_.mean }
  $p99s = $r | ForEach-Object { [double]$_.p99 }
  $frames = ($r | ForEach-Object { [int]$_.frames } | Measure-Object -Sum).Sum
  $mm = ($means | Measure-Object -Average -Minimum -Maximum)
  $pm = ($p99s | Measure-Object -Average).Average
  $dl = @($d | Where-Object { $_.file -like "composite_${m}_r*" -and $_.mode -ne $null -and [int]$_.frames -gt 0 })
  $cpu = '-'; $acq = '-'; $gd = '-'; $gc = '-'
  if ($dl.Count -gt 0 -and $m -like 'composite*') {
    $cpu = '{0:F4} / {1:F4}' -f (($dl | ForEach-Object { [double]$_.comp_mean } | Measure-Object -Average).Average), (($dl | ForEach-Object { [double]$_.comp_p99 } | Measure-Object -Average).Average)
    $acq = '{0:F3}' -f (($dl | ForEach-Object { [double]$_.acq_mean } | Measure-Object -Average).Average)
    $gd = '{0:F4}' -f (($dl | ForEach-Object { [double]$_.gpu_draw_mean } | Measure-Object -Average).Average)
    $gc = '{0:F4}' -f (($dl | Where-Object { [int]$_.gpu_copy_n -gt 0 } | ForEach-Object { [double]$_.gpu_copy_mean } | Measure-Object -Average).Average)
  }
  "| $m | $($r.Count) | $frames | {0:F4} | {1:F4} - {2:F4} | {3:F4} | {4:F0} | $cpu | $acq | $gd | $gc |" -f $mm.Average, $mm.Minimum, $mm.Maximum, $pm, (1000 / $mm.Average)
}
