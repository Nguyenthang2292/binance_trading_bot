$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "tunnel_lib.ps1")

if (-not (Test-Path -LiteralPath $script:StatePath)) {
    Write-Host "tunnel: not running"
    return
}

try {
    $s = Get-Content -LiteralPath $script:StatePath | ConvertFrom-Json
    $last = if ($s.last_check) { $s.last_check } else { $s.last_reconnect }
    Write-Host ("tunnel: {0} | egress {1} | reconnects {2} | last {3}" -f `
        $s.health, $s.egress_ip, $s.reconnect_count, $last)
}
catch {
    Write-Host "tunnel: state unreadable"
    throw
}
