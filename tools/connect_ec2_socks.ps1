param(
    [string]$EnvPath = (Join-Path (Split-Path $PSScriptRoot -Parent) ".env"),
    [string]$AwsCliPath = "",
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

function Resolve-AwsCli {
    param([string]$ExplicitPath)

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $candidates += $ExplicitPath
    }

    $commands = @()
    $commands += @(Get-Command aws.exe -All -ErrorAction SilentlyContinue)
    $commands += @(Get-Command aws -All -ErrorAction SilentlyContinue)
    foreach ($command in $commands) {
        if ($command.Source -and ($candidates -notcontains $command.Source)) {
            $candidates += $command.Source
        }
    }

    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            continue
        }

        try {
            $versionOutput = & $candidate --version 2>&1
        }
        catch {
            continue
        }
        if ($LASTEXITCODE -eq 0 -and (($versionOutput | Out-String) -match "aws-cli/")) {
            return $candidate
        }
    }

    throw @"
AWS CLI v2 for Windows was not found or is not executable.

Install it from https://aws.amazon.com/cli/ or pass the full path:
  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\connect_ec2_socks.ps1 -AwsCliPath "C:\Program Files\Amazon\AWSCLIV2\aws.exe"

If Get-Command aws resolves to C:\Users\Admin\.local\bin\aws, that file is a WSL bash shim and cannot be used by this PowerShell script.
"@
}

function ConvertTo-ProcessArgument {
    param([string]$Value)

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

function Invoke-NativeCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$FailureMessage
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = ($Arguments | ForEach-Object { ConvertTo-ProcessArgument -Value $_ }) -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    if ($process.ExitCode -ne 0) {
        $message = (($stdout, $stderr) -join "`n").Trim()
        if ([string]::IsNullOrWhiteSpace($message)) {
            $message = "No stderr/stdout was captured."
        }
        throw "$FailureMessage`n$message"
    }
    return $stdout
}

function Invoke-Aws {
    param([string[]]$Arguments)

    return Invoke-NativeCommand `
        -FilePath $script:AwsCli `
        -Arguments $Arguments `
        -FailureMessage "aws command failed: `"$script:AwsCli`" $($Arguments -join ' ')"
}

Import-DotEnv -Path $EnvPath
$script:AwsCli = Resolve-AwsCli -ExplicitPath $AwsCliPath
Write-Output "Using AWS CLI: $script:AwsCli"

if ([string]::IsNullOrWhiteSpace($Region)) {
    $Region = if ($AvailabilityZone -match "^(.+-\d)[a-z]$") {
        $Matches[1]
    } elseif ($env:AWS_REGION) {
        $env:AWS_REGION
    } elseif ($env:AWS_DEFAULT_REGION) {
        $env:AWS_DEFAULT_REGION
    } else {
        "ap-southeast-1"
    }
}
Write-Output "Using AWS region: $Region"

$existing = Get-NetTCPConnection -LocalPort $SocksPort -State Listen -ErrorAction SilentlyContinue
if ($existing) {
    $owners = ($existing | Select-Object -ExpandProperty OwningProcess -Unique) -join ", "
    Write-Output "SOCKS port $SocksPort is already listening. Owning process: $owners"
    exit 0
}

$currentIp = (Invoke-RestMethod -Uri "https://checkip.amazonaws.com").Trim()
$cidr = "$currentIp/32"

try {
    Invoke-Aws -Arguments @(
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

Invoke-NativeCommand `
    -FilePath "ssh-keygen.exe" `
    -Arguments @("-t", "ed25519", "-N", "", "-f", $keyPath, "-C", "codex-eic-temp") `
    -FailureMessage "ssh-keygen failed" | Out-Null

Invoke-Aws -Arguments @(
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
    "-o", "BatchMode=yes",
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
    "-o", "StrictHostKeyChecking=accept-new",
    "$OsUser@$PublicIp"
)

$sshOutPath = Join-Path $keyDir "ssh_tunnel.out"
$sshErrPath = Join-Path $keyDir "ssh_tunnel.err"
Remove-Item -LiteralPath $sshOutPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $sshErrPath -Force -ErrorAction SilentlyContinue

$process = Start-Process `
    -FilePath "ssh.exe" `
    -ArgumentList $sshArgs `
    -WindowStyle Hidden `
    -RedirectStandardOutput $sshOutPath `
    -RedirectStandardError $sshErrPath `
    -PassThru

$listener = $null
for ($attempt = 0; $attempt -lt 15; ++$attempt) {
    $listener = Get-NetTCPConnection -LocalPort $SocksPort -State Listen -ErrorAction SilentlyContinue
    if ($listener) {
        break
    }
    $process.Refresh()
    if ($process.HasExited) {
        break
    }
    Start-Sleep -Seconds 1
}
if (-not $listener) {
    $process.Refresh()
    $sshOutput = @()
    if (Test-Path -LiteralPath $sshOutPath) {
        $sshOutput += Get-Content -LiteralPath $sshOutPath -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $sshErrPath) {
        $sshOutput += Get-Content -LiteralPath $sshErrPath -ErrorAction SilentlyContinue
    }
    $detail = ($sshOutput | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($detail)) {
        $detail = "No ssh stdout/stderr was captured."
    }
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    throw "Tunnel process started as $($process.Id), but port $SocksPort is not listening`n$detail"
}

$proxyIp = (& curl.exe --socks5-hostname "127.0.0.1:$SocksPort" --max-time 15 "https://checkip.amazonaws.com").Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Tunnel started, but proxy verification failed"
}

Write-Output "SOCKS tunnel is listening on 127.0.0.1:$SocksPort"
Write-Output "SSH process id: $($process.Id)"
Write-Output "Proxy egress IP: $proxyIp"
