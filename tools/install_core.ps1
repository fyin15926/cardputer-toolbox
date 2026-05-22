$ErrorActionPreference = 'Continue'
$cli = "D:\github仓库同步\小机器\tools\arduino-cli\arduino-cli.exe"
$logDir = "D:\github仓库同步\小机器\tools"

# Be patient with flaky international links
& $cli config set network.connection_timeout 1200s | Out-Null

function Install-WithRetry($desc, [string[]]$cliArgs) {
  for ($i = 1; $i -le 6; $i++) {
    Write-Output "=== $desc attempt $i ==="
    $out = Join-Path $logDir "$desc.out.log"
    $err = Join-Path $logDir "$desc.err.log"
    $p = Start-Process -FilePath $cli -ArgumentList $cliArgs -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput $out -RedirectStandardError $err
    if ($p.ExitCode -eq 0) { Write-Output "=== $desc SUCCESS ==="; return $true }
    Write-Output "=== $desc failed (exit $($p.ExitCode)), retrying in 6s ==="
    Start-Sleep -Seconds 6
  }
  Write-Output "=== $desc GAVE UP ==="
  return $false
}

$coreOk = Install-WithRetry "core" @('core', 'install', 'esp32:esp32')
$libOk  = Install-WithRetry "lib"  @('lib', 'install', 'M5Cardputer')
Write-Output "FINAL coreOk=$coreOk libOk=$libOk"
Write-Output "ALL DONE"
