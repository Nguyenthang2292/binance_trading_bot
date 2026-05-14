$kv = @{}
Get-Content ".env" | ForEach-Object {
    if ($_ -match "^[A-Za-z_][A-Za-z0-9_]*=") {
        $parts = $_ -split "=", 2
        $kv[$parts[0]] = $parts[1]
    }
}

$awsText = @(
    "AWS_ACCESS_KEY_ID=$($kv['AWS_ACCESS_KEY_ID'])"
    "AWS_SECRET_ACCESS_KEY=$($kv['AWS_SECRET_ACCESS_KEY'])"
    "AWS_SESSION_TOKEN=$($kv['AWS_SESSION_TOKEN'])"
    "AWS_REGION=$($kv['AWS_REGION'])"
) -join "`n"
[System.IO.File]::WriteAllText("infra/aws.env.local", $awsText + "`n", [System.Text.Encoding]::ASCII)

$prodText = @(
    "BINANCE_API_KEY=$($kv['API_KEY'])"
    "BINANCE_SECRET_KEY=$($kv['API_SECRET'])"
) -join "`n"
[System.IO.File]::WriteAllText("infra/prod.env", $prodText + "`n", [System.Text.Encoding]::ASCII)

Write-Output "rewrote env files with LF"
