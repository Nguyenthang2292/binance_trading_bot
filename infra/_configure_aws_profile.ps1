$kv = @{}
Get-Content ".env" | ForEach-Object {
    $line = $_.Trim()
    if ($line -match "^[A-Za-z_][A-Za-z0-9_]*=") {
        $parts = $line -split "=", 2
        $kv[$parts[0]] = $parts[1]
    }
}

$aws = "C:\Program Files\Amazon\AWSCLIV2\aws.exe"
& $aws configure set aws_access_key_id $kv["AWS_ACCESS_KEY_ID"]
& $aws configure set aws_secret_access_key $kv["AWS_SECRET_ACCESS_KEY"]
if ($kv["AWS_SESSION_TOKEN"]) {
    & $aws configure set aws_session_token $kv["AWS_SESSION_TOKEN"]
}
& $aws configure set region $kv["AWS_REGION"]
Write-Output "configured aws default profile"
