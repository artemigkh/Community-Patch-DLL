# soak.ps1 - long run of server + host with ResizeBuffers churn; samples
# private bytes / working set / handles of both processes every 30 s into
# runs\<Name>\mem.csv. Only touches the two processes it started.
param([int]$Seconds = 600, [int]$Churn = 20, [string]$HostArgs = '--vsync 1', [string]$Name = 'soak')
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'bin'
$dir = Join-Path $root "runs\$Name"
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item "$dir\*" -ErrorAction SilentlyContinue
$srv = Start-Process "$bin\shell_server.exe" -ArgumentList "--rate 60 --log `"$dir\server.log`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1
$env:VPSHELL_LOG = "$dir\composite.log"
$h = Start-Process "$bin\test_host.exe" -ArgumentList "--seconds $Seconds --churn $Churn --label $Name --log `"$dir\host.log`" $HostArgs" -PassThru
Remove-Item Env:VPSHELL_LOG
$t0 = Get-Date
"t_s,host_private_mb,host_ws_mb,host_handles,server_private_mb,server_ws_mb,server_handles" | Out-File "$dir\mem.csv" -Encoding ascii
while (-not $h.HasExited) {
  $el = [int]((Get-Date) - $t0).TotalSeconds
  $h.Refresh(); $srv.Refresh()
  if ($h.HasExited) { break }
  "{0},{1:F2},{2:F2},{3},{4:F2},{5:F2},{6}" -f $el, ($h.PrivateMemorySize64/1MB), ($h.WorkingSet64/1MB), $h.HandleCount, ($srv.PrivateMemorySize64/1MB), ($srv.WorkingSet64/1MB), $srv.HandleCount | Out-File "$dir\mem.csv" -Append -Encoding ascii
  $next = 30 - ((Get-Date) - $t0).TotalSeconds % 30
  $h.WaitForExit([int]($next * 1000)) | Out-Null
}
Stop-Process -Id $srv.Id
Get-Content "$dir\mem.csv"
