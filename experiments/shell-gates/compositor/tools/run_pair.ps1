# run_pair.ps1 - start shell_server.exe (optional) and test_host.exe, take
# screenshots at given times, wait for the host to exit, stop the server.
# Logs land in runs\<Name>\ (server.log, host.log, composite.log).
# Never touches the game: it only starts and stops its own processes by PID.
param(
  [Parameter(Mandatory)][string]$Name,
  [string]$HostArgs = '',
  [string]$ServerArgs = '--rate 60',
  [switch]$NoServer,
  [int]$Seconds = 10,
  [double[]]$ShotAt = @(),
  [hashtable]$Env = @{},
  [string]$ShotPrefix = ''
)
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'bin'
$runs = Join-Path $root "runs\$Name"
New-Item -ItemType Directory -Force $runs | Out-Null
Remove-Item "$runs\*.log" -ErrorAction SilentlyContinue
$srv = $null
if (-not $NoServer) {
  $srv = Start-Process "$bin\shell_server.exe" -ArgumentList "$ServerArgs --log `"$runs\server.log`"" -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 700
}
$saved = @{}
$Env['VPSHELL_LOG'] = "$runs\composite.log"
foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$Env[$k]) }
$h = Start-Process "$bin\test_host.exe" -ArgumentList "--seconds $Seconds --label $Name --log `"$runs\host.log`" $HostArgs" -PassThru
foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
$t0 = Get-Date
$i = 0
foreach ($s in $ShotAt) {
  $wait = $s - ((Get-Date) - $t0).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
  $pfx = if ($ShotPrefix) { $ShotPrefix } else { $Name }
  & "$PSScriptRoot\shot.ps1" -ProcessId $h.Id -Out (Join-Path $root "shots\$pfx-t$([int]$s).png") -ClientOnly
  $i++
}
$h.WaitForExit(($Seconds + 30) * 1000) | Out-Null
if (-not $h.HasExited) { Stop-Process -Id $h.Id; 'host had to be killed' }
if ($srv -and -not $srv.HasExited) { Stop-Process -Id $srv.Id }
"host exit code $($h.ExitCode); logs in $runs"
