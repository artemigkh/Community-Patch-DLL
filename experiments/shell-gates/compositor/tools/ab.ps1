# ab.ps1 - frame-time A/B of the test host: no DLL / off / passthrough /
# composite(clear) / composite(present), interleaved over several rounds so a
# drifting background load (other GPU users) hits every mode equally.
# Each run is a fresh test_host process; the server keeps running throughout.
# Result: runs\ab\summary.csv (host-measured, first 3 s of each run excluded)
#         runs\ab\dll_stats.csv (per-second DLL stats lines, parsed)
param([int]$Seconds = 30, [int]$Rounds = 2, [string]$HostArgs = '--vsync 0 --bb 2560x1440', [string]$ServerArgs = '--rate 60', [string]$Out = 'ab')
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'bin'
$dir = Join-Path $root "runs\$Out"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*" -ErrorAction SilentlyContinue
$modes = @(
  @{ L = 'nodll';            A = '--load none'; E = @{} },
  @{ L = 'off';              A = '';            E = @{ VPSHELL_MODE = 'off' } },
  @{ L = 'passthrough';      A = '';            E = @{ VPSHELL_MODE = 'passthrough' } },
  @{ L = 'composite_clear';  A = '';            E = @{ VPSHELL_MODE = 'composite'; VPSHELL_INSERT = 'clear' } },
  @{ L = 'composite_present';A = '';            E = @{ VPSHELL_MODE = 'composite'; VPSHELL_INSERT = 'present' } }
)
$srv = Start-Process "$bin\shell_server.exe" -ArgumentList "$ServerArgs --size 2560x1440 --log `"$dir\server.log`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1
for ($r = 1; $r -le $Rounds; $r++) {
  foreach ($m in $modes) {
    $label = "$($m.L)"
    $m.E['VPSHELL_LOG'] = "$dir\composite_$($m.L)_r$r.log"
    $saved = @{}
    foreach ($k in $m.E.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$m.E[$k]) }
    $h = Start-Process "$bin\test_host.exe" -ArgumentList "--seconds $Seconds --label $label --summary `"$dir\summary.csv`" --log `"$dir\host_$($m.L)_r$r.log`" $HostArgs $($m.A)" -PassThru
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    $h.WaitForExit(($Seconds + 30) * 1000) | Out-Null
    if (-not $h.HasExited) { Stop-Process -Id $h.Id; "killed $label" }
    "round $r $label done"
  }
}
Stop-Process -Id $srv.Id
# parse DLL stats
$rows = foreach ($f in Get-ChildItem "$dir\composite_*.log") {
  foreach ($line in Get-Content $f) {
    if ($line -notmatch ' stats ') { continue }
    $o = [ordered]@{ file = $f.Name }
    foreach ($m in [regex]::Matches($line, '(\w+)=([\d\.]+|\w+)')) { $o[$m.Groups[1].Value] = $m.Groups[2].Value }
    [pscustomobject]$o
  }
}
$rows | Export-Csv "$dir\dll_stats.csv" -NoTypeInformation
"done: $dir"
