# stats_summary.ps1 - aggregate the per-second DLL stats lines of one or more runs
# (skipping the first $Skip seconds). Means of per-second means; p99 = mean of per-second p99s;
# counters summed.
param([Parameter(Mandatory)][string[]]$Runs, [int]$Skip = 3)
$root = Split-Path -Parent $PSScriptRoot
foreach ($r in $Runs) {
  $rows = foreach ($line in Get-Content (Join-Path $root "runs\$r\composite.log")) {
    if ($line -notmatch ' stats ') { continue }
    $o = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([\d\.]+)')) { $o[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
    $o
  }
  $rows = @($rows | Select-Object -Skip $Skip)
  $avg = { param($k) ($rows | ForEach-Object { $_[$k] } | Measure-Object -Average).Average }
  $sum = { param($k) ($rows | ForEach-Object { $_[$k] } | Measure-Object -Sum).Sum }
  $mx = { param($k) ($rows | ForEach-Object { $_[$k] } | Measure-Object -Maximum).Maximum }
  [pscustomobject]@{
    run = $r; seconds = $rows.Count; frames = (& $sum 'frames')
    ft_mean = '{0:F3}' -f (& $avg 'ft_mean'); ft_p99 = '{0:F3}' -f (& $avg 'ft_p99'); ft_max = '{0:F2}' -f (& $mx 'ft_max')
    comp_mean = '{0:F4}' -f (& $avg 'comp_mean'); comp_p99 = '{0:F4}' -f (& $avg 'comp_p99')
    acq_mean = '{0:F3}' -f (& $avg 'acq_mean'); acq_p99 = '{0:F3}' -f (& $avg 'acq_p99')
    gpu_draw = '{0:F4}' -f (& $avg 'gpu_draw_mean'); gpu_copy = '{0:F4}' -f (& $avg 'gpu_copy_mean'); gpu_copy_p99 = '{0:F3}' -f (& $avg 'gpu_copy_p99')
    lat_mean = '{0:F2}' -f (& $avg 'lat_mean'); lat_p99 = '{0:F2}' -f (& $avg 'lat_p99'); age_mean = '{0:F2}' -f (& $avg 'age_mean')
    srv_new = (& $sum 'srv_new'); srv_drop = (& $sum 'srv_drop'); dup = (& $sum 'dup'); timeouts = (& $sum 'timeouts'); missed = (& $sum 'missed_ins')
  }
}
