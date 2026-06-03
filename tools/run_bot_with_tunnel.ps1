param(
    [string]$BotExe = ".\build\windows-msvc-debug\bin\Debug\binance_trading_bot.exe",
    [string]$EnvPath = ".\.env"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$lib = Join-Path $PSScriptRoot "tunnel_lib.ps1"
. $lib

$script:SupervisorProcess = $null
$script:CleanupDone = $false
$script:TunnelSessionOwned = $false

function Stop-TunnelSession {
    if (-not $script:TunnelSessionOwned -and -not $script:SupervisorProcess) {
        return
    }
    if ($script:CleanupDone) {
        return
    }
    $script:CleanupDone = $true

    if ($script:SupervisorProcess) {
        Stop-Process -Id $script:SupervisorProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Stop-StaleSsh
    Remove-Item -LiteralPath $script:StatePath -Force -ErrorAction SilentlyContinue
    Write-Host "Tunnel + supervisor torn down."
}

$cancelHandler = [System.ConsoleCancelEventHandler]{
    param($sender, $eventArgs)
    Stop-TunnelSession
    $eventArgs.Cancel = $false
}
[System.Console]::add_CancelKeyPress($cancelHandler)

try {
    $envFile = Resolve-TunnelPath -Path $EnvPath
    Import-DotEnv -Path $envFile
    foreach ($k in "BINANCE_API_KEY", "BINANCE_SECRET_KEY") {
        if (-not [Environment]::GetEnvironmentVariable($k)) {
            throw "Missing $k in .env"
        }
    }

    $botPath = Resolve-TunnelPath -Path $BotExe
    if (-not (Test-Path -LiteralPath $botPath -PathType Leaf)) {
        throw "Bot executable not found: $botPath"
    }

    $script:TunnelSessionOwned = $true
    Stop-StaleSsh
    if (Test-Path -LiteralPath $script:StatePath) {
        $old = Get-Content -LiteralPath $script:StatePath | ConvertFrom-Json
        foreach ($oldPid in @($old.ssh_pid, $old.supervisor_pid)) {
            if ($null -eq $oldPid) {
                continue
            }
            $p = Get-Process -Id $oldPid -ErrorAction SilentlyContinue
            if ($p -and (@("ssh", "powershell", "pwsh") -contains $p.ProcessName)) {
                Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
            }
        }
        Remove-Item -LiteralPath $script:StatePath -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-TunnelEgress).Ok) {
        $connectScript = Join-Path $PSScriptRoot "connect_ec2_socks.ps1"
        $powershell = Get-TunnelPowerShellPath
        & $powershell -NoProfile -ExecutionPolicy Bypass -File $connectScript
        $initialHealth = Wait-TunnelEgress -Attempts 6 -DelaySec 2 -TimeoutSec 15
        if (-not $initialHealth.Ok) {
            throw "Initial tunnel failed egress check: expected=$script:ExpectedIp actual='$($initialHealth.Ip)' curl_exit=$($initialHealth.ExitCode)"
        }
    }

    $supervisorScript = Join-Path $PSScriptRoot "tunnel_supervisor.ps1"
    $connectScript = Join-Path $PSScriptRoot "connect_ec2_socks.ps1"
    $powershell = Get-TunnelPowerShellPath
    $script:SupervisorProcess = Start-Process `
        -FilePath $powershell `
        -PassThru `
        -WindowStyle Hidden `
        -ArgumentList (Join-TunnelProcessArguments -Arguments @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", $supervisorScript,
            "-LibPath", $lib,
            "-ConnectScript", $connectScript
        ))

    $env:BINANCE_SOCKS5_PROXY = "socks5://127.0.0.1:$script:SocksPort"
    $env:BINANCE_REQUIRE_SOCKS5_PROXY = "1"

    # Run bot with CWD pinned to repo root (parity with the runbook's `cd <repo-root>`),
    # so trading_bot.log and relative config paths resolve correctly.
    Push-Location $root
    try {
        & $botPath
    }
    finally {
        Pop-Location
    }
}
finally {
    [System.Console]::remove_CancelKeyPress($cancelHandler)
    Stop-TunnelSession
}
