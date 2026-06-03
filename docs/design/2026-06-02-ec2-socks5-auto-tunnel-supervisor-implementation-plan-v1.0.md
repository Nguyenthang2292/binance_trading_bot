# EC2 SOCKS5 Auto-Tunnel Supervisor — Implementation Plan v1.0

- **Date:** 2026-06-02
- **Implements:** [Design v1.1](2026-06-02-ec2-socks5-auto-tunnel-supervisor-v1.0.md)
- **Audience:** Self-implementation (operator codes this directly)
- **Runtime:** Windows PowerShell 5.1 / PowerShell 7 — no new runtime
- **Reuses verbatim:** [`tools/connect_ec2_socks.ps1`](../../tools/connect_ec2_socks.ps1), [`infra/binance-bot-key.pem`](../../infra/binance-bot-key.pem)

> ⚠️ This is live-trading infrastructure (egress must stay on EC2 IP `47.131.185.59`).
> Build and test on **testnet** (`BINANCE_TESTNET=1`) before any live run.

---

## 0. Pre-flight (do these first — they are blockers)

| Step | Action | Verify |
|------|--------|--------|
| 0.1 | Add Binance keys to `.env` (already gitignored ✅) | `BINANCE_API_KEY=...`, `BINANCE_SECRET_KEY=...`, optional `BINANCE_TESTNET=1` |
| 0.2 | Confirm persistent key works manually | `ssh -i infra\binance-bot-key.pem -o BatchMode=yes ubuntu@47.131.185.59 echo ok` |
| 0.3 | Create `logs/` if missing | `New-Item -ItemType Directory -Force logs` |
| 0.4 | Add `.tunnel_state.json` + `logs/tunnel_supervisor.log*` to `.gitignore` | `git check-ignore .tunnel_state.json` |

---

## 1. Deliverables (files to create)

```
tools/
  run_bot_with_tunnel.ps1     # NEW — parent launcher (owns lifecycle)
  tunnel_supervisor.ps1       # NEW — supervisor loop (run as child/job)
  tunnel_lib.ps1              # NEW — shared helpers (SSH args, egress check, status I/O)
  tunnel_status.ps1           # NEW — one-line human status reader (U1)
  connect_ec2_socks.ps1       # EXISTING — unchanged (full reconnect path)
```

**Why `tunnel_lib.ps1`:** single source of truth for SSH options + egress check +
status-file read/write, dot-sourced by the other three. Closes objection **C4**
(option drift) and makes the supervisor testable in isolation.

---

## 2. `tools/tunnel_lib.ps1` (shared helpers)

Dot-sourced by everything else. Pure functions, no side effects at import.

```powershell
$script:ExpectedIp   = "47.131.185.59"
$script:SocksPort    = 1080
$script:RepoRoot     = Split-Path $PSScriptRoot -Parent
$script:PersistKey   = Join-Path $RepoRoot "infra\binance-bot-key.pem"
$script:KnownHosts   = Join-Path $RepoRoot "infra\.known_hosts"   # C3, repo-local
$script:StatePath    = Join-Path $RepoRoot ".tunnel_state.json"
$script:LogPath      = Join-Path $RepoRoot "logs\tunnel_supervisor.log"

function Get-TunnelSshArgs {                      # C4 — single source of truth
    param([string]$KeyPath)
    @(
        "-i", $KeyPath,
        "-D", "$script:SocksPort",
        "-N",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "BatchMode=yes",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=2",            # C1 — tighter SLA
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "UserKnownHostsFile=`"$script:KnownHosts`"",
        "ubuntu@$script:ExpectedIp"
    )
}

function Test-TunnelEgress {                       # health check (egress IP)
    param([int]$TimeoutSec = 15)
    $ip = (& curl.exe --socks5-hostname "127.0.0.1:$script:SocksPort" `
                      --max-time $TimeoutSec "https://checkip.amazonaws.com" 2>$null).Trim()
    return [pscustomobject]@{ Ok = ($ip -eq $script:ExpectedIp); Ip = $ip }
}

function Get-SocksOwnerPid {                        # port owner pid or $null
    (Get-NetTCPConnection -LocalPort $script:SocksPort -State Listen -EA SilentlyContinue |
        Select-Object -First 1 -Expand OwningProcess)
}

function Stop-StaleSsh {                            # S4 — match ssh AND port owner
    $procPid = Get-SocksOwnerPid
    if (-not $procPid) { return }
    $p = Get-Process -Id $procPid -EA SilentlyContinue
    if ($p -and $p.ProcessName -eq "ssh") {
        Stop-Process -Id $procPid -Force -EA SilentlyContinue
    }
}

function Write-TunnelStatus {                       # atomic write (temp + rename)
    param([hashtable]$Fields)
    $tmp = "$script:StatePath.tmp"
    ($Fields | ConvertTo-Json) | Set-Content -LiteralPath $tmp -Encoding utf8
    Move-Item -LiteralPath $tmp -Destination $script:StatePath -Force
}

function Write-TunnelLog {                          # C5 — rotate at ~5MB
    param([string]$Msg)
    if ((Test-Path $script:LogPath) -and (Get-Item $script:LogPath).Length -gt 5MB) {
        Move-Item $script:LogPath "$script:LogPath.old" -Force
    }
    "$([DateTimeOffset]::Now.ToString('o'))  $Msg" |
        Add-Content -LiteralPath $script:LogPath -Encoding utf8
}
```

**Note (S6):** these `$script:` vars are computed from `$PSScriptRoot`, so they
resolve correctly **inside the supervisor child** because the child dot-sources
this file by absolute path. Do not rely on the caller's cwd anywhere.

---

## 3. `tools/tunnel_supervisor.ps1` (the loop)

Runs as a **hidden child PowerShell process** (preferred over `Start-Job` so it
survives independently and is testable standalone). Takes absolute paths as params.

```powershell
param(
    [Parameter(Mandatory)][string]$LibPath,        # absolute path to tunnel_lib.ps1
    [string]$ConnectScript,                        # absolute path to connect_ec2_socks.ps1
    [int]$IntervalSec = 15                          # C1
)
. $LibPath
$ErrorActionPreference = "Stop"
$lastFull = [DateTime]::MinValue
$reconnectCount = 0

function Try-LightReconnect {
    Write-TunnelLog "light reconnect via persistent key"
    Stop-StaleSsh
    $args = Get-TunnelSshArgs -KeyPath $script:PersistKey
    $p = Start-Process ssh.exe -ArgumentList $args -WindowStyle Hidden -PassThru
    for ($i=0; $i -lt 10; $i++) {
        if (Get-SocksOwnerPid) { break }
        if ($p.HasExited) { return $null }
        Start-Sleep -Milliseconds 800
    }
    $h = Test-TunnelEgress
    if ($h.Ok) { return $p.Id }
    Stop-Process -Id $p.Id -Force -EA SilentlyContinue   # half-open / wrong route
    return $null
}

function Try-FullReconnect {
    if (([DateTime]::Now - $script:lastFull).TotalSeconds -lt 30) { return $null }  # C2
    Write-TunnelLog "full reconnect via connect_ec2_socks.ps1"
    Stop-StaleSsh
    & powershell -NoProfile -ExecutionPolicy Bypass -File $ConnectScript | Out-Null
    $script:lastFull = [DateTime]::Now
    $h = Test-TunnelEgress
    if ($h.Ok) { return (Get-SocksOwnerPid) }
    return $null
}

while ($true) {
    $h = Test-TunnelEgress
    if ($h.Ok) {
        Write-TunnelStatus @{ health="OK"; egress_ip=$h.Ip; expected_ip=$script:ExpectedIp
            ssh_pid=(Get-SocksOwnerPid); supervisor_pid=$PID
            last_check=[DateTimeOffset]::Now.ToString('o'); reconnect_count=$reconnectCount }
        Start-Sleep -Seconds $IntervalSec
        continue
    }

    Write-TunnelLog "UNHEALTHY (egress='$($h.Ip)') — reconnecting"
    Write-TunnelStatus @{ health="RECONNECTING"; egress_ip=$h.Ip; supervisor_pid=$PID
        last_check=[DateTimeOffset]::Now.ToString('o'); reconnect_count=$reconnectCount }

    $delays = @(5,10,30,60)
    $idx = 0
    while ($true) {
        $pid = Try-LightReconnect
        if (-not $pid) { Write-TunnelLog "light failed -> escalating to full"; $pid = Try-FullReconnect }  # S2
        if ($pid) {
            $reconnectCount++
            Write-TunnelLog "tunnel restored (ssh_pid=$pid)"
            Write-TunnelStatus @{ health="OK"; egress_ip=$script:ExpectedIp; ssh_pid=$pid
                supervisor_pid=$PID; last_reconnect=[DateTimeOffset]::Now.ToString('o')
                reconnect_count=$reconnectCount }
            break
        }
        $d = $delays[[Math]::Min($idx,$delays.Count-1)]; $idx++
        Write-TunnelLog "reconnect attempt failed; retry in ${d}s"
        Start-Sleep -Seconds $d
    }
}
```

---

## 4. `tools/run_bot_with_tunnel.ps1` (parent launcher)

Owns the lifecycle. **Order matters (U2).**

```powershell
param(
    [string]$BotExe = ".\build\windows-msvc-debug\bin\Debug\binance_trading_bot.exe",
    [string]$EnvPath = ".\.env"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot | Split-Path -Parent
$lib  = Join-Path $PSScriptRoot "tunnel_lib.ps1"
. $lib

# --- U2: validate .env FIRST, before opening any tunnel ---
Import-DotEnv -Path (Join-Path $root $EnvPath)          # reuse loader from connect script
foreach ($k in "BINANCE_API_KEY","BINANCE_SECRET_KEY") {
    if (-not [Environment]::GetEnvironmentVariable($k)) { throw "Missing $k in .env" }
}

# --- S4: orphan reaper (match ssh AND port owner) ---
Stop-StaleSsh
if (Test-Path $script:StatePath) {
    $old = Get-Content $script:StatePath | ConvertFrom-Json
    foreach ($pid in @($old.ssh_pid,$old.supervisor_pid)) {
        $p = Get-Process -Id $pid -EA SilentlyContinue
        if ($p -and $p.ProcessName -in "ssh","powershell","pwsh") { Stop-Process -Id $pid -Force -EA SilentlyContinue }
    }
    Remove-Item $script:StatePath -Force -EA SilentlyContinue
}

$supervisor = $null
try {
    # --- ensure tunnel: reuse if healthy, else full connect ---
    if (-not (Test-TunnelEgress).Ok) {
        & powershell -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $PSScriptRoot "connect_ec2_socks.ps1")
        if (-not (Test-TunnelEgress).Ok) { throw "Initial tunnel failed egress check" }
    }

    # --- start supervisor as hidden child (S6: absolute paths) ---
    $supervisor = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        "-NoProfile","-ExecutionPolicy","Bypass","-File",(Join-Path $PSScriptRoot "tunnel_supervisor.ps1"),
        "-LibPath",$lib,
        "-ConnectScript",(Join-Path $PSScriptRoot "connect_ec2_socks.ps1")
    )

    # --- bot env + run in foreground ---
    $env:BINANCE_SOCKS5_PROXY = "socks5://127.0.0.1:$script:SocksPort"
    $env:BINANCE_REQUIRE_SOCKS5_PROXY = "1"
    & (Join-Path $root $BotExe)        # blocks until bot exits
}
finally {                              # U4: also reached on Ctrl-C (see note)
    if ($supervisor) { Stop-Process -Id $supervisor.Id -Force -EA SilentlyContinue }
    Stop-StaleSsh
    Remove-Item $script:StatePath -Force -EA SilentlyContinue
    Write-Host "Tunnel + supervisor torn down."
}
```

**U4 — Ctrl-C caveat (must test live):** In PowerShell, Ctrl-C on a native
foreground process *usually* runs `finally`, but verify. If it does not, add at
top of script:
```powershell
[Console]::TreatControlCAsInput = $false
$null = Register-EngineEvent PowerShell.Exiting -Action { Stop-StaleSsh }
```
or wrap the bot call in a `trap { ... ; break }`. **Do not ship until the
Ctrl-C path is confirmed to leave no orphan `ssh.exe` on :1080.**

---

## 5. `tools/tunnel_status.ps1` (U1)

```powershell
. (Join-Path $PSScriptRoot "tunnel_lib.ps1")
if (-not (Test-Path $script:StatePath)) { Write-Host "tunnel: not running"; return }
$s = Get-Content $script:StatePath | ConvertFrom-Json
Write-Host ("tunnel: {0} | egress {1} | reconnects {2} | last {3}" -f `
    $s.health, $s.egress_ip, $s.reconnect_count, $s.last_check)
```

---

## 6. Build sequence (suggested order)

1. `tunnel_lib.ps1` — write + unit-poke each function in a REPL.
2. `tunnel_status.ps1` — trivial, lets you observe state during later tests.
3. `tunnel_supervisor.ps1` — run **standalone** against an already-open tunnel; kill
   ssh by hand and watch it heal. This isolates S6 before wiring the parent.
4. `run_bot_with_tunnel.ps1` — wire it all; test full happy path on **testnet**.
5. Confirm teardown (normal exit + Ctrl-C) leaves no `ssh.exe` on :1080.

---

## 7. Test checklist (maps to Design §6 + review)

- [ ] **Happy path** (testnet): launch → `Using SOCKS5 proxy ... 1080` in `trading_bot.log`.
- [ ] **Reuse**: tunnel already healthy → no duplicate ssh, bot starts fast.
- [ ] **S6**: kill supervisor's inherited-context assumption — run supervisor standalone, confirm it reconnects (paths resolve).
- [ ] **Half-open**: freeze ssh / block egress → supervisor sees wrong/empty IP, `Stop-StaleSsh`, heals; bot stays alive.
- [ ] **Light→full escalation (S2)**: remove `/32` ingress in SG → light fails, log shows escalation, full re-adds ingress.
- [ ] **C2**: trigger two failures <30s apart → only one full reconnect fires.
- [ ] **S4 reaper**: Task-Manager-kill the parent, relaunch → no orphan, clean :1080.
- [ ] **U2**: rename a key out of `.env` → fails *before* tunnel opens.
- [ ] **U3**: correct egress + Binance `-2015` → supervisor does **not** reconnect.
- [ ] **U4**: Ctrl-C the parent → no `ssh.exe` left on :1080.
- [ ] **C5**: force log >5MB → rotation to `.old`.

---

## 8. Out of scope (do not build now)

- Windows boot auto-start / Service (explicit non-goal).
- Crash-vs-deliberate-stop discrimination (rejected in design).
- Alerting beyond local log + status file.
