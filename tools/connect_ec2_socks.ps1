param(
    [string]$EnvPath = (Join-Path (Split-Path $PSScriptRoot -Parent) ".env"),
    [string]$Region = "",
    [string]$InstanceId = "i-0aa87ef3250b32640",
    [string]$AvailabilityZone = "ap-southeast-1c",
    [string]$SecurityGroupId = "sg-000e0643aadaa1f9b",
    [string]$PublicIp = "47.131.185.59",
    [string]$OsUser = "ubuntu",
    [int]$SocksPort = 1080
)

$ErrorActionPreference = "Stop"

function Import-DotEnv {
    param([string]$Path)

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

function Invoke-Aws {
    param([string[]]$Args)

    $output = & aws @Args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $message = ($output | Out-String).Trim()
        throw "aws command failed: aws $($Args -join ' ')`n$message"
    }
    return $output
}

Import-DotEnv -Path $EnvPath

if ([string]::IsNullOrWhiteSpace($Region)) {
    $Region = if ($env:AWS_REGION) { $env:AWS_REGION } else { "ap-southeast-1" }
}

$existing = Get-NetTCPConnection -LocalPort $SocksPort -State Listen -ErrorAction SilentlyContinue
if ($existing) {
    $owners = ($existing | Select-Object -ExpandProperty OwningProcess -Unique) -join ", "
    Write-Output "SOCKS port $SocksPort is already listening. Owning process: $owners"
    exit 0
}

$currentIp = (Invoke-RestMethod -Uri "https://checkip.amazonaws.com").Trim()
$cidr = "$currentIp/32"

try {
    Invoke-Aws -Args @(
        "ec2", "authorize-security-group-ingress",
        "--region", $Region,
        "--group-id", $SecurityGroupId,
        "--ip-permissions", "IpProtocol=tcp,FromPort=22,ToPort=22,IpRanges=[{CidrIp=$cidr,Description='SSH from current local IP'}]"
    ) | Out-Null
    Write-Output "Added SSH ingress for $cidr"
}
catch {
    if ($_.Exception.Message -notmatch "InvalidPermission.Duplicate") {
        throw
    }
    Write-Output "SSH ingress already exists for $cidr"
}

$tcp = Test-NetConnection -ComputerName $PublicIp -Port 22 -InformationLevel Quiet
if (-not $tcp) {
    throw "TCP/22 is not reachable on $PublicIp"
}

$keyDir = Join-Path $env:TEMP "codex-eic-binance"
$keyPath = Join-Path $keyDir "eic_temp_ed25519"
New-Item -ItemType Directory -Force -Path $keyDir | Out-Null
Remove-Item -LiteralPath $keyPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$keyPath.pub" -Force -ErrorAction SilentlyContinue

& ssh-keygen.exe -t ed25519 -N "" -f $keyPath -C "codex-eic-temp" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "ssh-keygen failed"
}

Invoke-Aws -Args @(
    "ec2-instance-connect", "send-ssh-public-key",
    "--region", $Region,
    "--instance-id", $InstanceId,
    "--availability-zone", $AvailabilityZone,
    "--instance-os-user", $OsUser,
    "--ssh-public-key", "file://$keyPath.pub"
) | Out-Null

$sshArgs = @(
    "-i", $keyPath,
    "-D", "$SocksPort",
    "-N",
    "-o", "ExitOnForwardFailure=yes",
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
    "-o", "StrictHostKeyChecking=accept-new",
    "$OsUser@$PublicIp"
)

$process = Start-Process -FilePath "ssh.exe" -ArgumentList $sshArgs -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 2

$listener = Get-NetTCPConnection -LocalPort $SocksPort -State Listen -ErrorAction SilentlyContinue
if (-not $listener) {
    throw "Tunnel process started as $($process.Id), but port $SocksPort is not listening"
}

$proxyIp = (& curl.exe --socks5-hostname "127.0.0.1:$SocksPort" --max-time 15 "https://checkip.amazonaws.com").Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Tunnel started, but proxy verification failed"
}

Write-Output "SOCKS tunnel is listening on 127.0.0.1:$SocksPort"
Write-Output "SSH process id: $($process.Id)"
Write-Output "Proxy egress IP: $proxyIp"
