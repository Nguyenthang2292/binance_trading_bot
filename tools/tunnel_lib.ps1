$script:ExpectedIp = "47.131.185.59"
$script:SocksPort = 1080
$script:RepoRoot = Split-Path $PSScriptRoot -Parent
$script:PersistKey = Join-Path $script:RepoRoot "infra\binance-bot-key.pem"
$script:StatePath = Join-Path $script:RepoRoot ".tunnel_state.json"
$script:LogPath = Join-Path $script:RepoRoot "logs\tunnel_supervisor.log"
# Note: default ssh known_hosts is used (consistent with connect_ec2_socks.ps1).
# A repo-local known_hosts is avoided because the repo path contains spaces,
# which ssh's UserKnownHostsFile would split into multiple filenames.

function Resolve-TunnelPath {
    param([Parameter(Mandatory)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $script:RepoRoot $Path
}

function Import-DotEnv {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Env file not found: $Path"
    }

    Get-Content -LiteralPath $Path | ForEach-Object {
        $line = $_.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith("#")) {
            return
        }

        $parts = $line -split "=", 2
        if ($parts.Count -ne 2) {
            return
        }

        $name = $parts[0].Trim()
        $value = $parts[1].Trim()

        if (($value.StartsWith('"') -and $value.EndsWith('"')) -or
            ($value.StartsWith("'") -and $value.EndsWith("'"))) {
            $value = $value.Substring(1, $value.Length - 2)
        }

        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

function ConvertTo-TunnelProcessArgument {
    param([AllowNull()][string]$Value)

    if ($null -eq $Value -or $Value.Length -eq 0) {
        return '""'
    }
    if ($Value -notmatch '[\s"]') {
        return $Value
    }

    $result = '"'
    $backslashes = 0
    foreach ($char in $Value.ToCharArray()) {
        if ($char -eq '\') {
            ++$backslashes
            continue
        }
        if ($char -eq '"') {
            $result += ('\' * (($backslashes * 2) + 1))
            $result += '"'
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            $result += ('\' * $backslashes)
            $backslashes = 0
        }
        $result += $char
    }
    if ($backslashes -gt 0) {
        $result += ('\' * ($backslashes * 2))
    }
    $result += '"'
    return $result
}

function Join-TunnelProcessArguments {
    param([Parameter(Mandatory)][string[]]$Arguments)

    return ($Arguments | ForEach-Object { ConvertTo-TunnelProcessArgument -Value $_ }) -join " "
}

function Get-TunnelPowerShellPath {
    if ($PSVersionTable.PSEdition -eq "Core") {
        $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
        if ($pwsh) {
            return $pwsh.Source
        }
    }

    $powershell = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($powershell) {
        return $powershell.Source
    }

    return "powershell.exe"
}

function Get-TunnelSshArgs {
    param([Parameter(Mandatory)][string]$KeyPath)

    @(
        "-i", $KeyPath,
        "-D", "$script:SocksPort",
        "-N",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "BatchMode=yes",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=2",
        "-o", "StrictHostKeyChecking=accept-new",
        "ubuntu@$script:ExpectedIp"
    )
}

function Start-TunnelSsh {
    param([Parameter(Mandatory)][string]$KeyPath)

    $sshArgs = Get-TunnelSshArgs -KeyPath $KeyPath
    return Start-Process `
        -FilePath "ssh.exe" `
        -ArgumentList (Join-TunnelProcessArguments -Arguments $sshArgs) `
        -WindowStyle Hidden `
        -PassThru
}

function Test-TunnelEgress {
    param([int]$TimeoutSec = 15)

    $exitCode = $null
    try {
        $raw = & curl.exe --silent --show-error --socks5-hostname "127.0.0.1:$script:SocksPort" `
                          --max-time $TimeoutSec "https://checkip.amazonaws.com" 2>$null
        $exitCode = $LASTEXITCODE
        $ip = (($raw | Out-String).Trim())
    }
    catch {
        $ip = ""
    }

    return [pscustomobject]@{
        Ok = ($ip -eq $script:ExpectedIp)
        Ip = $ip
        ExitCode = $exitCode
    }
}

function Wait-TunnelEgress {
    param(
        [int]$Attempts = 5,
        [int]$DelaySec = 2,
        [int]$TimeoutSec = 15
    )

    $last = $null
    for ($i = 1; $i -le $Attempts; ++$i) {
        $last = Test-TunnelEgress -TimeoutSec $TimeoutSec
        if ($last.Ok) {
            return $last
        }
        if ($i -lt $Attempts) {
            Start-Sleep -Seconds $DelaySec
        }
    }
    return $last
}

function Get-SocksOwnerPid {
    $conn = Get-NetTCPConnection `
        -LocalPort $script:SocksPort `
        -State Listen `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if (-not $conn) {
        return $null
    }
    return $conn.OwningProcess
}

function Stop-StaleSsh {
    $procPid = Get-SocksOwnerPid
    if (-not $procPid) {
        return
    }

    $p = Get-Process -Id $procPid -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -eq "ssh") {
        Stop-Process -Id $procPid -Force -ErrorAction SilentlyContinue
    }
}

function Write-TunnelStatus {
    param([Parameter(Mandatory)][hashtable]$Fields)

    $tmp = "$script:StatePath.tmp"
    ($Fields | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath $tmp -Encoding utf8
    Move-Item -LiteralPath $tmp -Destination $script:StatePath -Force
}

function Write-TunnelLog {
    param([Parameter(Mandatory)][string]$Msg)

    $logDir = Split-Path $script:LogPath -Parent
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    if ((Test-Path -LiteralPath $script:LogPath) -and
        (Get-Item -LiteralPath $script:LogPath).Length -gt 5MB) {
        Move-Item -LiteralPath $script:LogPath -Destination "$script:LogPath.old" -Force
    }

    "$([DateTimeOffset]::Now.ToString('o'))  $Msg" |
        Add-Content -LiteralPath $script:LogPath -Encoding utf8
}
