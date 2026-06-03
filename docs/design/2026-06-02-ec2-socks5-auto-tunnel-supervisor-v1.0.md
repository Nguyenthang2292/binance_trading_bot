# EC2 SOCKS5 Auto-Tunnel Supervisor — Design v1.0

- **Date:** 2026-06-02
- **Status:** v1.1 — Multi-agent peer review complete; Arbiter disposition **REVISE → resolved**. Ready for implementation.
- **Owner:** Justinnguyen2292
- **Related:** [`tools/connect_ec2_socks.ps1`](../../tools/connect_ec2_socks.ps1), [`docs/spec/local-ec2-socks5-runbook.html`](../spec/local-ec2-socks5-runbook.html), [`infra/binance-bot-key.pem`](../../infra/binance-bot-key.pem)

---

## 1. Understanding Summary

- **What:** An automation + supervision layer wrapped around the existing
  [`connect_ec2_socks.ps1`](../../tools/connect_ec2_socks.ps1), collapsing today's
  two manual steps (open SSH SOCKS tunnel by hand + start bot by hand in a second
  terminal) into **one launch action**.
- **Why:** Remove manual steps, prevent forgetting a step, and **self-heal** when
  the SSH tunnel drops while the bot is trading.
- **Who:** The operator running the bot locally on Windows, where every Binance
  REST/WebSocket connection must egress through the EC2 Elastic IP
  `47.131.185.59` (whitelisted in Binance).
- **Flow:** Launch → ensure tunnel ready (open if missing) → **verify egress IP
  equals the EC2 IP** → set env from `.env` → run bot. A background supervisor
  watches the tunnel; on failure it **heals silently on the same port 1080** so
  the bot is not interrupted.
- **Lifecycle:** One session = one lifecycle. Bot stops/crashes → clean up tunnel
  - supervisor, leaving no orphan `ssh.exe`.

### Non-goals

- ❌ No auto-start on Windows boot.
- ❌ Not a resident Windows Service / NSSM.
- ❌ No change to the bot's trading logic.
- ❌ Does not touch the legacy `BinanceAPI` code path.

---

## 2. Assumptions (Non-Functional)

| # | Assumption | Value |
|---|------------|-------|
| 1 | Health-check interval | Egress IP check every **30 s** |
| 2 | Reconnect backoff | 5s → 10s → 30s → 60s (cap 60s), **unbounded retries**, each attempt logged |
| 3 | Bot secrets source | `BINANCE_API_KEY`, `BINANCE_SECRET_KEY`, optional `BINANCE_TESTNET` added to `.env` alongside AWS creds |
| 4 | Bot exe path | Default `.\build\windows-msvc-debug\bin\Debug\binance_trading_bot.exe`, overridable by parameter |
| 5 | Runtime | **PowerShell** only (consistent with existing script); no new runtime |
| 6 | Logging | Supervisor logs to `logs/tunnel_supervisor.log`, separate from `trading_bot.log` |
| 7 | Window visibility | Tunnel + supervisor run hidden/background; bot runs in the main foreground window |

---

## 3. Decision Log

| Decision | Alternatives considered | Why chosen |
|----------|------------------------|------------|
| **One-launch model, service-like behind a single command** | (a) Resident service auto-starting at Windows boot; (b) Tunnel as independent background service, bot manual | Operator wants to press one thing and have the tunnel come up *then* the bot. No need for boot survival. Simplest to control and debug. |
| **On tunnel drop while trading: heal underneath, keep bot alive** | (a) Restart whole bot after tunnel returns; (b) Alert only | Bot reconnects its own REST/WS once port 1080 returns; avoids losing WS streams / re-warmup. Accepted risk: a brief window of failing requests during reconnect. |
| **Bot stop/crash → tear down tunnel + supervisor** | (a) Keep tunnel running; (b) Distinguish crash vs deliberate stop | "One session = one lifecycle." Clean, no orphan `ssh.exe`. Simpler than crash/stop discrimination. |
| **Health check via egress IP through proxy** | (a) Process + port only; (b) Two-tier combo | Catches "half-open" tunnels (port still listening but SSH hung). Worth ~1 request per cycle. |
| **Secrets from `.env`** | (a) Windows User/Machine env; (b) Pass at runtime | Single source of config alongside the AWS creds the script already reads. |
| **Light SSH-only reconnect first, full script only when needed** | Always run full `connect_ec2_socks.ps1` | Full path re-adds ingress + mints a fresh EC2 Instance Connect (EIC) temp key every time. Light path reuses the persistent key + existing ingress — faster and lighter when the local IP has not changed. |
| **Status file for PID/health** | Read logs only | Lets the operator glance at "is the tunnel OK?" without parsing logs. |
| **Architecture: single parent launcher owns lifecycle (Approach A)** | (b) Decoupled background supervisor process; (c) Windows Service / Scheduled Task | Parent process owns the lifecycle; children die with it, matching "tunnel tắt theo bot." Reuses existing script nearly verbatim. Adds a small orphan-reaper to cover hard-kill of the parent window. |

---

## 4. Key Technical Insight — Two Reconnect Paths

The existing `connect_ec2_socks.ps1` authenticates using an **EC2 Instance
Connect (EIC) temporary key**, which is only valid for ~60 seconds to *establish*
a connection. It therefore **cannot be reused** for a later light reconnect.

A persistent key exists at [`infra/binance-bot-key.pem`](../../infra/binance-bot-key.pem),
which is the correct tool for light reconnect.

| Path | Mechanism | When used |
|------|-----------|-----------|
| **Light reconnect** | `ssh -i infra\binance-bot-key.pem -D 1080 -N ...` reusing the existing security-group ingress | Local public IP unchanged; persistent key authorized. Fast. |
| **Full reconnect** | `connect_ec2_socks.ps1` (re-adds `/32` ingress for the new IP + mints EIC temp key + opens tunnel) | Local IP changed (SG blocks SSH → timeout), or light reconnect fails for any reason |

**Critical ordering rule:** On a *half-open* tunnel, port 1080 is **still
listening**, so `connect_ec2_socks.ps1` would early-exit with
`SOCKS port 1080 is already listening` and fix nothing. Therefore the supervisor
**must kill the stale `ssh.exe` first**, then attempt reconnect.

---

## 5. Final Design (Approach A)

### 5.1 Components

```
tools/run_bot_with_tunnel.ps1   (parent launcher — owns lifecycle)
   |
   |-- (1) reaper: clean orphan ssh.exe from prior hard-killed sessions
   |-- (2) ensure tunnel: reuse port 1080 if healthy, else full connect
   |-- (3) start supervisor as a background Job/runspace
   |-- (4) run bot in foreground (env loaded from .env)
   `-- (5) finally{}: stop supervisor + kill tunnel ssh.exe + remove status file

tools/connect_ec2_socks.ps1     (existing — full reconnect, reused as-is)
infra/binance-bot-key.pem       (existing — persistent key for light reconnect)
.env                            (AWS creds + Binance keys + optional BINANCE_TESTNET)
logs/tunnel_supervisor.log      (supervisor log)
.tunnel_state.json              (status file: pid, health, egress IP, last check)
```

### 5.2 Process model & data flow

```
run_bot_with_tunnel.ps1 (parent, foreground console)
  │
  ├─ reap orphans ───────────────► kill stray ssh.exe bound to :1080 from old PID file
  │
  ├─ ensure tunnel ──────────────► egress-check :1080
  │        healthy? ── yes ───────► reuse
  │        no ────────────────────► connect_ec2_socks.ps1 (full)
  │
  ├─ spawn supervisor job ───────►  loop { every 30s: egress-check
  │                                      ok   → write OK to status, sleep
  │                                      fail → kill ssh, light reconnect,
  │                                             else full reconnect (backoff),
  │                                             write status each attempt }
  │
  ├─ load .env into process env, set:
  │     BINANCE_SOCKS5_PROXY=socks5://127.0.0.1:1080
  │     BINANCE_REQUIRE_SOCKS5_PROXY=1
  │
  ├─ run bot exe (foreground, blocks) ── operator sees trade output
  │
  └─ finally: Stop-Job supervisor; kill tunnel ssh.exe; delete .tunnel_state.json
```

### 5.3 Supervisor loop (pseudocode)

```text
loop:
    ip = curl --socks5-hostname 127.0.0.1:1080 --max-time 15 https://checkip.amazonaws.com
    if ip == "47.131.185.59":
        write_status(health="OK", egress=ip, ts=now, ssh_pid=<pid>)
        sleep 30
        continue

    # unhealthy (timeout, wrong IP, or half-open)
    log("tunnel unhealthy: got '{ip}'")
    write_status(health="RECONNECTING", ts=now)
    kill_stale_ssh()                       # MUST kill first (half-open still listens)

    for delay in [5, 10, 30, 60, 60, ...]: # unbounded, capped at 60
        if try_light_reconnect():          # ssh -i infra\binance-bot-key.pem -D 1080 -N
            break
        if try_full_reconnect():           # connect_ec2_socks.ps1
            break
        log("reconnect attempt failed; retrying in {delay}s")
        write_status(health="RECONNECTING", attempt=n)
        sleep delay

    log("tunnel restored")
    write_status(health="OK", ...)
```

> Note: `try_light_reconnect` opens its own hidden `ssh.exe` with the persistent
> key, then verifies egress; if the verify fails it kills that process and returns
> false so the loop falls through to `try_full_reconnect`.

### 5.4 Status file (`.tunnel_state.json`)

```json
{
  "health": "OK",
  "egress_ip": "47.131.185.59",
  "expected_ip": "47.131.185.59",
  "ssh_pid": 12345,
  "supervisor_pid": 6789,
  "last_check": "2026-06-02T10:15:30+07:00",
  "last_reconnect": null,
  "reconnect_count": 0
}
```

- Written atomically (temp file + rename) on every check and every reconnect step.
- Health values: `OK` | `RECONNECTING` | `DOWN` (initial/teardown).
- Lets the operator run a one-liner (`Get-Content .tunnel_state.json | ConvertFrom-Json`)
  to see status without reading logs.

### 5.5 Cleanup & orphan reaper

- **Normal exit / Ctrl-C:** parent's `try/finally` stops the supervisor job, kills
  the tunnel `ssh.exe` (by PID recorded in status file), and deletes the status file.
- **Hard kill of the parent window** (e.g. Task Manager): `finally` may not run.
  Mitigation: on next launch, **step (1) reaper** reads any leftover
  `.tunnel_state.json`, and if its `ssh_pid` / `supervisor_pid` still exist and are
  bound to port 1080, kills them before starting fresh. This guarantees a clean
  port 1080 at every launch.

### 5.6 Error handling

| Failure | Handling |
|---------|----------|
| Initial tunnel cannot open (full connect throws) | Parent aborts before starting bot; clear error surfaced (script already captures SSH stdout/stderr) |
| Egress check returns wrong IP (not `47.131.185.59`) | Treated as unhealthy → reconnect cycle (covers half-open & wrong-route) |
| Local public IP changed mid-session | Light reconnect fails (SG blocks) → full reconnect re-adds `/32` ingress |
| Persistent key missing/unauthorized | Light reconnect fails → fall through to full reconnect (EIC) |
| `.env` missing Binance keys | Parent fails fast with explicit message before launching bot |
| Bot exits (any reason) | `finally` tears everything down (one session = one lifecycle) |

---

## 6. Testing Strategy

- **Happy path:** clean machine, run launcher → tunnel opens, egress verified, bot
  starts and logs `Using SOCKS5 proxy ... 127.0.0.1:1080`.
- **Reuse path:** tunnel already healthy on :1080 → launcher reuses, no duplicate ssh.
- **Half-open injection:** suspend/kill the SSH process' network without freeing the
  port (or block egress) → supervisor detects wrong/empty egress IP, kills stale ssh,
  reconnects; bot keeps running.
- **IP-change simulation:** remove the `/32` ingress rule in the SG → light reconnect
  fails → full reconnect re-adds ingress and restores tunnel.
- **Hard-kill reaper:** kill the parent window via Task Manager, relaunch → reaper
  cleans orphan ssh + stale status file, starts fresh.
- **Teardown:** stop the bot → confirm no `ssh.exe` on :1080 and status file removed.

---

## 7. Open Items for Implementation

- Confirm exact persistent-key SSH options to mirror the runbook
  (`ExitOnForwardFailure=yes`, `ServerAliveInterval=30`, `ServerAliveCountMax=3`,
  `BatchMode=yes`, `StrictHostKeyChecking=accept-new`).
- Decide whether the supervisor runs as a PowerShell background `Job` vs a hidden
  child `pwsh`/`powershell` process invoking a dedicated `tunnel_supervisor.ps1`
  (the latter keeps the loop testable in isolation; leaning that way).
- Parameterize `ExpectedIp`, `SocksPort`, `BotExe`, `EnvPath` with the same defaults
  as `connect_ec2_socks.ps1`.

---

## 8. Multi-Agent Peer Review (v1.1)

Reviewers invoked sequentially: Skeptic → Constraint Guardian → User Advocate →
Arbiter. Disposition: **REVISE** (architecture sound; objections folded in below).

### 8.1 Verified against codebase (blockers closed)

- **S1 — Guard is startup-only ✅.** [`src/main.cpp:866-877`](../../src/main.cpp#L866)
  checks `BINANCE_REQUIRE_SOCKS5_PROXY` once, before `BinanceContext` is built. The
  proxy `127.0.0.1:1080` is baked into the context once. A mid-session tunnel drop
  does **not** trip the guard; the bot keeps running and new REST/WS connections
  route through 1080 once it returns (WS auto-reconnects). **Residual:** in-flight
  REST during the gap may fail — that is the bot's own concern, not the tunnel's.
- **S3 — `.env` is gitignored ✅.** `git check-ignore .env` → `.env`
  ([`.gitignore:11`](../../.gitignore#L11)). Safe to store Binance secrets there.

### 8.2 Accepted objections → design changes

| ID | Change incorporated |
|----|---------------------|
| **S2** | When light reconnect fails, log explicitly `light reconnect failed → escalating to full`, so it never looks like a bug. |
| **S4** | Orphan reaper must match **`ProcessName == ssh` AND owns port 1080**, not PID alone (Windows reuses PIDs). |
| **S6** | Supervisor (background Job/child process) does **not** inherit cwd or vars. Pass **absolute paths** for `connect_ec2_socks.ps1`, `infra\binance-bot-key.pem`, `.env`, status/log paths, and `ExpectedIp` explicitly (via `$using:` / `-ArgumentList` / params). Otherwise the supervisor dies silently on first reconnect. |
| **C1** | Tighten recovery SLA: `ServerAliveCountMax=2` + supervisor egress check every **15 s** → worst-case blind window ~45–60 s (was ~120 s). Assumption #1 updated. |
| **C2** | Full reconnect throttled: minimum **30 s between two full attempts**; light-first always tried first; full only when light fails for network/SG reasons. Limits EIC API churn. |
| **C3** | Use a repo-local `known_hosts` with `StrictHostKeyChecking=accept-new`; on host-key mismatch, log a loud warning and surface it in the status file rather than retrying blindly. |
| **C4** | Single source of truth for SSH options: a shared `Get-TunnelSshArgs` helper (or shared variable) reused by both the light path and `connect_ec2_socks.ps1` semantics — avoid drift. |
| **C5** | `tunnel_supervisor.log` rotates at ~5 MB, keeping one `.old`. |
| **U1** | Add `tools/tunnel_status.ps1` — prints a one-line human-readable status from `.tunnel_state.json` (health, egress, last reconnect, count). |
| **U2** | **Order of operations:** validate `.env` (keys present) → reaper → ensure tunnel → start supervisor → run bot. Fail fast on missing keys **before** opening the tunnel. |
| **U3** | Supervisor judges health **only** by egress IP == `47.131.185.59`. A Binance `-2015` with correct egress is **not** a tunnel fault and must not trigger reconnect. |
| **U4** | Ctrl-C must run cleanup: register a handler (`[Console]::CancelKeyPress` / `trap`/`finally`) so the tunnel `ssh.exe` + supervisor are torn down even on Ctrl-C. **Must be tested live.** |

### 8.3 Revised Assumptions (supersede §2)

- **#1 → Health-check interval = 15 s** (was 30 s); `ServerAliveCountMax = 2`.
- **#2 → Backoff** unchanged (5→10→30→60), unbounded, **plus** ≥30 s min spacing
  between full reconnects.
- All other assumptions (#3–#7) unchanged.

### 8.4 Updated reconnect ordering (supersedes §5.3 escalation)

```text
on unhealthy:
    write_status(RECONNECTING); kill_stale_ssh()   # half-open still listens → must kill
    if try_light_reconnect():            # ssh -i infra\binance-bot-key.pem (persistent)
        restored; return
    log("light reconnect failed → escalating to full")
    if (now - last_full) >= 30s:
        if try_full_reconnect(): restored; return   # connect_ec2_socks.ps1 (ingress+EIC)
    backoff(); retry
```

### 8.5 Exit criteria status

- Understanding Lock completed ✅
- All reviewer agents invoked ✅ (Skeptic, Guardian, Advocate, Arbiter)
- All objections resolved or explicitly dispositioned ✅
- Decision Log complete (§3) + review table (§8.2) ✅
- Arbiter declared design acceptable after revision ✅

**Final disposition: APPROVED (as v1.1).**

---

## 9. Implementation Deviations (post-code review)

Recorded after the implementation was written and reviewed, so the design matches
the shipped code.

| Area | Design said | Shipped code does | Why |
|------|-------------|-------------------|-----|
| **C3 — known_hosts** | Use a repo-local `infra\.known_hosts` with `StrictHostKeyChecking=accept-new` | **Dropped** `UserKnownHostsFile`; uses ssh's **default** known_hosts (same as `connect_ec2_socks.ps1`) | The repo path contains spaces (`D:\NGUYEN QUANG THANG\...`). ssh's `UserKnownHostsFile` treats its value as a **whitespace-separated list of files**, so a spaced path is split into bogus filenames. Embedding quotes was possible but fragile; using the default known_hosts is simpler and keeps both reconnect paths consistent. Host-key trust on first connect is still handled by `accept-new`. |
| **§5 — tunnel reuse** | "Ensure tunnel: reuse port 1080 if healthy, else full connect" | The launcher's **reaper always `Stop-StaleSsh` first**, so any pre-existing tunnel is torn down and a fresh one is opened every launch (no reuse) | Stronger guarantee of a clean port 1080 per launch, fully consistent with "one session = one lifecycle." Under normal operation the tunnel is already torn down on bot exit, so there is rarely a healthy tunnel to reuse anyway. |

Both deviations were accepted during code review (see implementation plan v1.0).
