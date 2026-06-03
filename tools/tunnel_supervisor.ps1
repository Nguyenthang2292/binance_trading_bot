param(
    [Parameter(Mandatory)][string]$LibPath,
    [Parameter(Mandatory)][string]$ConnectScript,
    [int]$IntervalSec = 15
)

$ErrorActionPreference = "Stop"
. $LibPath

$script:lastFull = [DateTime]::MinValue
$script:reconnectCount = 0
$script:lastReconnectTs = $null

function Try-LightReconnect {
    Write-TunnelLog "light reconnect via persistent key"
    Stop-StaleSsh

    $p = Start-TunnelSsh -KeyPath $script:PersistKey
    for ($i = 0; $i -lt 10; ++$i) {
        if (Get-SocksOwnerPid) {
            break
        }
        $p.Refresh()
        if ($p.HasExited) {
            return $null
        }
        Start-Sleep -Milliseconds 800
    }

    $h = Test-TunnelEgress
    if ($h.Ok) {
        return $p.Id
    }

    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    return $null
}

function Try-FullReconnect {
    if (([DateTime]::Now - $script:lastFull).TotalSeconds -lt 30) {
        return $null
    }

    Write-TunnelLog "full reconnect via connect_ec2_socks.ps1"
    Stop-StaleSsh

    $powershell = Get-TunnelPowerShellPath
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $ConnectScript | Out-Null
    $script:lastFull = [DateTime]::Now

    $h = Test-TunnelEgress
    if ($h.Ok) {
        return (Get-SocksOwnerPid)
    }
    return $null
}

Write-TunnelLog "supervisor started pid=$PID interval=${IntervalSec}s"

while ($true) {
    $h = Test-TunnelEgress
    if ($h.Ok) {
        Write-TunnelStatus @{
            health = "OK"
            egress_ip = $h.Ip
            expected_ip = $script:ExpectedIp
            ssh_pid = (Get-SocksOwnerPid)
            supervisor_pid = $PID
            last_check = [DateTimeOffset]::Now.ToString('o')
            last_reconnect = $script:lastReconnectTs
            reconnect_count = $script:reconnectCount
        }
        Start-Sleep -Seconds $IntervalSec
        continue
    }

    Write-TunnelLog "UNHEALTHY (egress='$($h.Ip)') - reconnecting"
    Write-TunnelStatus @{
        health = "RECONNECTING"
        egress_ip = $h.Ip
        expected_ip = $script:ExpectedIp
        supervisor_pid = $PID
        last_check = [DateTimeOffset]::Now.ToString('o')
        last_reconnect = $script:lastReconnectTs
        reconnect_count = $script:reconnectCount
    }

    $delays = @(5, 10, 30, 60)
    $idx = 0
    while ($true) {
        $sshPid = Try-LightReconnect
        if (-not $sshPid) {
            Write-TunnelLog "light failed -> escalating to full"
            $sshPid = Try-FullReconnect
        }

        if ($sshPid) {
            ++$script:reconnectCount
            $script:lastReconnectTs = [DateTimeOffset]::Now.ToString('o')
            Write-TunnelLog "tunnel restored (ssh_pid=$sshPid)"
            Write-TunnelStatus @{
                health = "OK"
                egress_ip = $script:ExpectedIp
                expected_ip = $script:ExpectedIp
                ssh_pid = $sshPid
                supervisor_pid = $PID
                last_check = [DateTimeOffset]::Now.ToString('o')
                last_reconnect = $script:lastReconnectTs
                reconnect_count = $script:reconnectCount
            }
            break
        }

        $d = $delays[[Math]::Min($idx, $delays.Count - 1)]
        ++$idx
        Write-TunnelLog "reconnect attempt failed; retry in ${d}s"
        Start-Sleep -Seconds $d
    }
}
