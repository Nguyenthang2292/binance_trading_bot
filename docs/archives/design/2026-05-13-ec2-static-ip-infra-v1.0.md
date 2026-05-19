# EC2 Static IP Infrastructure v1.0

**Date:** 2026-05-13  
**Status:** ✅ DONE - Implementation scripts complete; production validation required  
**Scope:** Provision một EC2 Ubuntu 24.04 với Elastic IP làm static outbound IP cho Binance API whitelist và làm production runtime cho C++ trading bot.

---

## 1. Bối cảnh và Mục tiêu

Binance production API key cần whitelist IP tĩnh. Máy local dev có IP động nên không phù hợp để whitelist trực tiếp.

**Giải pháp v1.0:** EC2 + Elastic IP.

1. **Production runtime:** Bot C++ chạy trực tiếp trên EC2 bằng `systemd`. Outbound IP tới Binance là Elastic IP đã whitelist.
2. **Static IP endpoint:** Elastic IP là IP duy nhất cần whitelist trong Binance API Management.
3. **Local dev qua EC2:** Không được coi là supported cho bot hiện tại nếu chỉ đặt `ALL_PROXY=socks5h://localhost:1080`. Code network hiện dùng Boost.Asio/Beast raw TCP socket nên không tự đọc `ALL_PROXY`, `HTTPS_PROXY` hoặc biến proxy tương tự.

**Điều kiện để local bot thật sự đi qua EC2:**

- Implement SOCKS5 connector trong `HttpSession` và `WsSession`, hoặc
- Dùng VPN/TUN system-level routing như WireGuard/Tailscale, hoặc
- Chạy bản dev của bot trực tiếp trên EC2.

`ssh-proxy.sh` vẫn hữu ích cho `curl`, browser, hoặc tooling có hỗ trợ SOCKS5, nhưng không được document như cách chạy bot local cho tới khi transport layer hỗ trợ proxy.

**Không phải mục tiêu:**

- CI/CD pipeline tự động.
- Multi-instance, HA hoặc load balancing.
- Testnet infrastructure riêng.
- Container orchestration.
- Transparent proxy/VPN system-level routing trong v1.0.

---

## 2. Kiến trúc

```text
LOCAL DEV MACHINE
│
├─ Provision/Deploy:
│   infra/provision.sh -> AWS EC2/EIP/SG/key pair
│   infra/deploy.sh    -> rsync source -> build on EC2 -> restart systemd
│
├─ Optional proxy-compatible tooling:
│   infra/ssh-proxy.sh -> SOCKS5 localhost:1080
│   curl/browser/tooling with SOCKS5 support -> EC2 -> Binance
│
└─ Local C++ bot:
    NOT routed by ALL_PROXY in current implementation.
    Must use SOCKS5-aware transport, VPN/TUN, or run on EC2.

                    ┌─────────────────────────────┐
                    │  EC2 t3.micro or t3.small   │
                    │  Ubuntu 24.04 LTS           │
                    │  Elastic IP: x.x.x.x        │
                    │  Binance whitelists this IP │
                    │                             │
                    │  /opt/binance-bot/          │
                    │    binance_trading_bot      │
                    │    prod.env                 │
                    │                             │
                    │  systemd:                   │
                    │    binance-bot.service      │
                    └──────────────┬──────────────┘
                                   │ TCP 443
                                   ▼
                          BINANCE API / WS
```

---

## 3. Infrastructure Components

Chi phí phụ thuộc region và pricing thay đổi theo thời gian. Bảng dưới là estimate cho một instance chạy 24/7.

| Component | Spec | Chi phí ước tính |
|---|---|---:|
| EC2 Instance | `t3.micro`, Ubuntu 24.04 LTS, x86_64 | ~$8-10/tháng tùy region |
| Public IPv4 / Elastic IP | In-use hoặc idle đều bị tính phí | ~$3.6/tháng |
| EBS Root Volume | 20 GB gp3 | ~$1.6-2/tháng |
| Key Pair | RSA 4096, download 1 lần | Free |
| Security Group | SSH inbound restricted, egress controlled | Free |
| **Tổng running** | EC2 + IPv4 + EBS | **~$13-16/tháng** |

**Lưu ý:**

- Không giả định Elastic IP attached là free. AWS hiện tính phí public IPv4 theo giờ kể cả khi đang attached.
- `t3.micro` là burstable instance. Không ghi “không throttle”. Nếu dùng CPU credits `standard`, build C++ có thể chậm khi hết credit. Nếu dùng `unlimited`, có thể phát sinh surplus CPU credit charge.
- Nếu build trên EC2 quá chậm, nâng tạm lên `t3.small` hoặc chuyển sang WSL2/Docker Linux build rồi upload binary.

---

## 4. Security Group Rules

### 4.1 Inbound

| Direction | Protocol | Port | Source | Lý do |
|---|---|---:|---|---|
| Inbound | TCP | 22 | `MY_IP/32` | SSH, rsync, deploy, SOCKS tunnel optional |

Không mở inbound `80`, `443` hoặc port application khác.

### 4.2 Outbound

Implementation phải chọn rõ một trong hai mode, không để doc nói strict egress trong khi script vẫn dùng default all-outbound.

**Mode khuyến nghị cho v1.0: strict-by-port egress**

| Direction | Protocol | Port | Destination | Lý do |
|---|---|---:|---|---|
| Outbound | TCP | 443 | `0.0.0.0/0` | Binance REST/WS, apt HTTPS, GitHub FetchContent |
| Outbound | TCP | 80 | `0.0.0.0/0` | apt repo fallback nếu cần |
| Outbound | UDP | 123 | `0.0.0.0/0` | NTP time sync cho signed Binance requests |

**Yêu cầu implementation:**

1. Sau khi tạo Security Group, revoke default egress rule `0.0.0.0/0 all protocols`.
2. Add các egress rule trên bằng `authorize-security-group-egress`.
3. Không add `9443` trong v1.0. Code hiện kết nối Binance REST và WebSocket qua port `443`; chỉ thêm `9443` nếu transport hoặc endpoint thật sự đổi sang port đó.

**Mode thay thế: default all-outbound**

Chỉ chấp nhận nếu script và doc ghi rõ đây là tradeoff đơn giản hóa bootstrap/build. Không được mô tả là least privilege.

---

## 5. File Structure

```text
infra/
├── provision.sh          # Chạy 1 lần: tạo infrastructure
├── teardown.sh           # Xóa infrastructure, hỗ trợ partial state
├── deploy.sh             # rsync source -> build trên EC2 -> restart service
├── ssh-proxy.sh          # SOCKS5 tunnel cho tooling có hỗ trợ proxy
├── update-ssh-ip.sh      # Cập nhật SG inbound khi IP local thay đổi
├── prod.env.example      # Template runtime env, tracked
├── prod.env              # Binance runtime secrets, gitignored
├── aws.env.local         # AWS credentials local-only, gitignored
└── .ec2-state            # Auto-generated, gitignored
```

`.ec2-state` format:

```text
INSTANCE_ID=i-0123456789abcdef0
ELASTIC_IP=1.2.3.4
ALLOC_ID=eipalloc-0123456789abcdef0
KEY_NAME=binance-bot-key
KEY_PATH=~/.ssh/binance-bot-key.pem
SECURITY_GROUP_ID=sg-0123456789abcdef0
REGION=ap-southeast-1
PROVISIONED_AT=2026-05-13T00:00:00Z
MY_IP_AT_PROVISION=203.0.113.10
```

`.gitignore` phải có:

```gitignore
infra/.ec2-state
infra/aws.env.local
infra/prod.env
*.pem
```

**Không dùng root `.env` làm input cho cả AWS và production runtime.** Trộn AWS credentials và Binance credentials trong cùng file dễ dẫn tới upload nhầm AWS secrets lên EC2.

---

## 6. Secrets và Environment Boundary

### 6.1 Local AWS credentials

File: `infra/aws.env.local`

```bash
AWS_ACCESS_KEY_ID=...
AWS_SECRET_ACCESS_KEY=...
AWS_SESSION_TOKEN=...
AWS_REGION=ap-southeast-1
```

Chỉ các script quản lý AWS được load file này:

- `provision.sh`
- `teardown.sh`
- `update-ssh-ip.sh`

Không upload file này lên EC2.

### 6.2 Production runtime env

File: `infra/prod.env`

```bash
BINANCE_API_KEY=...
BINANCE_SECRET_KEY=...
```

`deploy.sh` upload file này tới `/opt/binance-bot/prod.env`.

Yêu cầu guardrail trong `deploy.sh`:

- Fail nếu `infra/prod.env` không tồn tại.
- Fail nếu `infra/prod.env` chứa dòng bắt đầu bằng `AWS_`.
- Set permission remote: `chmod 600 /opt/binance-bot/prod.env`.
- App hiện đọc đúng tên biến `BINANCE_API_KEY` và `BINANCE_SECRET_KEY`.

---

## 7. Chi tiết từng Script

### 7.1 `provision.sh`

**Điều kiện tiên quyết:**

- `aws-cli` v2.
- AWS credentials trong `infra/aws.env.local` hoặc đã export sẵn trong environment.
- SSH client, `curl`.

**Luồng thực thi:**

```text
1. Load AWS credentials từ infra/aws.env.local nếu file tồn tại.
2. Resolve REGION từ AWS_REGION, fallback ap-southeast-1.
3. Lấy MY_IP tự động và validate IPv4.
4. Tạo hoặc fail-fast nếu key pair / SG / state đã tồn tại.
5. Ghi .ec2-state incremental sau mỗi resource được tạo để teardown có thể cleanup khi provision fail giữa chừng.
6. Tạo Key Pair:
   - aws ec2 create-key-pair --key-name binance-bot-key
   - Lưu private key -> ~/.ssh/binance-bot-key.pem
   - chmod 400
7. Tạo Security Group:
   - Inbound TCP 22 từ MY_IP/32.
   - Revoke default all-outbound nếu dùng strict-by-port mode.
   - Add outbound TCP 443, TCP 80, UDP 123.
8. Lookup latest Ubuntu 24.04 LTS AMI.
9. Launch EC2:
   - Instance type: t3.micro hoặc t3.small.
   - EBS: 20 GB gp3, DeleteOnTermination=true.
   - Tag toàn bộ resource có thể tag: Name=binance-bot, App=binance-bot, ManagedBy=infra-script.
   - Explicit CPU credit mode: standard để cost predictable, hoặc unlimited nếu có alarm/budget.
10. Wait instance running.
11. Allocate Elastic IP.
12. Associate Elastic IP với instance.
13. Wait SSH available.
14. Bootstrap qua SSH:
    - apt update
    - install build-essential, cmake, git, libssl-dev, rsync, ninja-build, ca-certificates
    - tạo user/group system `binance-bot`
    - tạo /opt/binance-bot/
    - set ownership/permission phù hợp
    - tạo systemd service file
    - systemctl enable binance-bot, chưa start
15. In Elastic IP cần whitelist trên Binance.
```

**Failure handling:**

- Nếu fail sau khi tạo resource, `.ec2-state` partial vẫn phải đủ thông tin để `teardown.sh` cleanup.
- `teardown.sh` cũng phải có fallback discover resource bằng tag nếu state thiếu.

### 7.2 `ssh-proxy.sh`

**Mục đích:** Mở SOCKS5 proxy cho tooling có hỗ trợ SOCKS5.

```bash
ssh -i "$KEY_PATH" \
    -D 1080 \
    -N \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o ExitOnForwardFailure=yes \
    "ubuntu@$ELASTIC_IP"
```

Script chỉ nên in hướng dẫn kiểu này:

```text
SOCKS5 proxy running on localhost:1080.
This works only for clients that explicitly support SOCKS5.
Current C++ bot does not use ALL_PROXY/HTTPS_PROXY automatically.

Proxy-capable smoke test:
HTTPS_PROXY=socks5h://localhost:1080 curl https://fapi.binance.com/fapi/v1/ping
```

Không document lệnh `ALL_PROXY=socks5h://localhost:1080 ./binance_trading_bot` cho tới khi code có SOCKS5-aware transport hoặc system-level VPN.

### 7.3 `deploy.sh`

**Mục đích:** Upload source, build Linux binary trên EC2, upload runtime env, restart service.

V1.0 chọn **build trên EC2**, không build Windows binary local rồi upload.

```text
1. Load infra/.ec2-state.
2. Verify SSH connectivity.
3. Verify infra/prod.env tồn tại và không chứa AWS_ variables.
4. Upload infra/prod.env -> /opt/binance-bot/prod.env.
5. chmod 600 /opt/binance-bot/prod.env.
6. rsync source lên ~/binance-bot-src/:
   - exclude build/
   - exclude .git/
   - exclude .env
   - exclude infra/.ec2-state
   - exclude infra/aws.env.local
   - exclude infra/prod.env
   - exclude *.pem
7. Build trên EC2:
   - cmake -S ~/binance-bot-src -B ~/binance-bot-build -DCMAKE_BUILD_TYPE=Release
   - cmake --build ~/binance-bot-build --target binance_trading_bot --parallel "$(nproc)"
8. Optional pre-restart validation:
   - ctest --test-dir ~/binance-bot-build --output-on-failure nếu test build được enable.
9. Install binary:
   - sudo install -o binance-bot -g binance-bot -m 755 ... /opt/binance-bot/binance_trading_bot
10. Restart:
   - sudo systemctl restart binance-bot
11. Show status:
   - sudo systemctl status binance-bot --no-pager -l
```

### 7.4 `teardown.sh`

**Mục đích:** Xóa toàn bộ infrastructure để ngừng tính phí.

```text
1. Confirm từ user bằng cách type "yes".
2. Load infra/aws.env.local nếu có.
3. Load infra/.ec2-state nếu có.
4. Nếu state thiếu, discover resource bằng tag App=binance-bot và ManagedBy=infra-script.
5. Disassociate + release Elastic IP nếu tồn tại.
6. Terminate EC2 instance nếu tồn tại.
7. Wait instance terminated.
8. Delete Security Group nếu tồn tại.
9. Delete Key Pair AWS side nếu tồn tại.
10. Không tự xóa local private key, chỉ in path để user tự quyết.
11. Xóa .ec2-state sau khi cleanup hoàn tất.
```

Script phải idempotent ở mức best-effort: resource đã bị xóa thủ công không làm teardown fail toàn bộ.

### 7.5 `update-ssh-ip.sh`

**Mục đích:** Khi IP local thay đổi, cập nhật Security Group inbound SSH.

```text
1. Load infra/aws.env.local nếu có.
2. Load infra/.ec2-state.
3. Lấy IP mới qua external IP service và validate IPv4.
4. List tất cả inbound rules TCP 22 hiện có trên Security Group.
5. Revoke các rule cũ do script quản lý.
6. Authorize rule mới MY_IP/32.
7. Update MY_IP_AT_PROVISION trong .ec2-state.
```

Không tự mở `0.0.0.0/0` trừ khi user explicitly override bằng flag riêng.

---

## 8. systemd Service trên EC2

Provision tạo dedicated Linux user thay vì chạy bot bằng `ubuntu`.

```ini
[Unit]
Description=Binance Trading Bot
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=binance-bot
Group=binance-bot
WorkingDirectory=/opt/binance-bot
EnvironmentFile=/opt/binance-bot/prod.env
ExecStart=/opt/binance-bot/binance_trading_bot
Restart=on-failure
RestartSec=10s
TimeoutStopSec=30s
KillSignal=SIGTERM
StandardOutput=journal
StandardError=journal
SyslogIdentifier=binance-bot

# Basic hardening. Keep /opt/binance-bot writable because the current app writes trading_bot.log there.
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=/opt/binance-bot

[Install]
WantedBy=multi-user.target
```

Commands hay dùng:

```bash
sudo systemctl status binance-bot
sudo systemctl start binance-bot
sudo systemctl stop binance-bot
sudo journalctl -u binance-bot -f
```

---

## 9. Binance IP Whitelist

Sau khi `provision.sh` hoàn thành, script in:

```text
ELASTIC IP: x.x.x.x
Whitelist this IP in Binance API Management before starting the bot.
```

Các bước trên Binance:

1. Đăng nhập Binance.
2. Vào API Management.
3. Chọn API key production.
4. Edit restrictions.
5. Restrict access to trusted IPs only.
6. Thêm Elastic IP.
7. Lưu và xác nhận 2FA.

Chỉ chạy production bot sau khi whitelist hoàn tất.

---

## 10. Workflow hằng ngày

### Scenario A: Deploy production runtime

```bash
./infra/deploy.sh

ssh -i ~/.ssh/binance-bot-key.pem ubuntu@<elastic-ip> \
    "sudo journalctl -u binance-bot -f"
```

### Scenario B: Smoke test qua SOCKS5 cho client có hỗ trợ proxy

```bash
./infra/ssh-proxy.sh

HTTPS_PROXY=socks5h://localhost:1080 \
    curl https://fapi.binance.com/fapi/v1/ping
```

Không dùng scenario này để kết luận bot local đã đi qua EC2 nếu bot chưa có SOCKS5-aware transport.

### Scenario C: Local IP thay đổi

```bash
./infra/update-ssh-ip.sh
```

### Scenario D: Không dùng lâu dài

```bash
./infra/teardown.sh
```

Teardown là cách duy nhất chắc chắn để dừng chi phí EC2, EBS và public IPv4.

---

## 11. Decision Log

| Quyết định | Các lựa chọn | Lý do chọn |
|---|---|---|
| AWS CLI script thay vì Terraform | Terraform, CDK, AWS CLI | Scope nhỏ. Chỉ chấp nhận nếu script có tag, incremental state và teardown idempotent. Nếu infra tăng scope, chuyển sang Terraform/CDK. |
| EC2 + Elastic IP | NAT Gateway, VPN VPS, residential IP | Đơn giản, outbound IP ổn định, trực tiếp phù hợp Binance whitelist. |
| `t3.micro` mặc định | `t3.small`, `t4g.micro`, Lightsail | Runtime nhẹ. Build C++ có thể chậm do CPU credit; nếu chậm, nâng instance hoặc build Linux binary ngoài EC2. |
| Ubuntu 24.04 LTS | Amazon Linux 2023, Debian 12 | Toolchain và package quen thuộc, phù hợp C++23 với gcc/clang mới. |
| Build trên EC2 cho v1.0 | Windows build, WSL2, Docker buildx | Tránh cross-compile Windows -> Linux. Deploy chậm hơn nhưng ít moving parts. |
| SOCKS5 SSH tunnel chỉ cho proxy-aware tools | VPN/TUN, SOCKS5 trong app | SSH có sẵn nhưng không transparent. Không claim local bot dùng được proxy cho tới khi transport hỗ trợ. |
| systemd | screen/tmux, supervisor | Restart policy, journald, chuẩn Linux service. |
| Dedicated `binance-bot` user | Chạy bằng `ubuntu` | Giảm blast radius nếu bot/process bị compromise. |
| Split env files | Một root `.env` chung | Tránh upload nhầm AWS credentials lên EC2. |
| Strict-by-port egress | Default all-outbound | Giảm surface area. Nếu script giữ default all-outbound thì phải document như tradeoff, không gọi là least privilege. |

---

## 12. Rủi ro và Giới hạn

| Rủi ro | Mức độ | Giải pháp |
|---|---|---|
| Local bot bypass EC2 dù đặt `ALL_PROXY` | Cao | Không document workflow này. Implement SOCKS5-aware transport, VPN/TUN, hoặc chạy bot trên EC2. |
| Upload nhầm AWS credentials lên EC2 | Cao | Split `infra/aws.env.local` và `infra/prod.env`; `deploy.sh` reject `AWS_` trong prod env. |
| Provision fail giữa chừng để lại resource tính phí | Cao | Incremental `.ec2-state`, tags, teardown discover-by-tag, cleanup best-effort. |
| AWS session token expire khi provision/deploy | Trung bình | Load credentials trước khi chạy, fail fast bằng `aws sts get-caller-identity`. |
| Public IPv4/EIP phát sinh phí cả khi attached | Trung bình | Cost note rõ ràng; teardown khi không dùng. |
| `t3.micro` hết CPU credit khi build | Trung bình | Dùng build cache, nâng tạm instance, hoặc chuyển sang WSL2/Docker build. |
| Single EC2 là SPOF | Trung bình | Chấp nhận trong v1.0; nếu cần HA thì thiết kế lại state, deployment và failover. |
| Clock drift gây lỗi signed request | Trung bình | Ensure NTP/chrony hoạt động, outbound UDP 123 nếu cần. |
| Secrets nằm plain text trên disk | Trung bình | `chmod 600`, dedicated user, không commit secrets. Nếu scope tăng, dùng AWS Secrets Manager hoặc SSM Parameter Store. |

---

## 13. Cross-Compilation và Build

Dự án dev chính trên Windows/MSVC, production chạy Linux. V1.0 chọn **build trên EC2**.

Các option:

| Option | Mô tả | Đánh giá |
|---|---|---|
| A | Upload source và build trên EC2 | Chọn cho v1.0. Đơn giản nhất, nhưng build chậm trên `t3.micro`. |
| B | Build trong WSL2 Ubuntu rồi upload binary | Tốt hơn nếu deploy thường xuyên, cần setup WSL2 toolchain. |
| C | Docker buildx/Linux container | Reproducible hơn, nhưng thêm Docker dependency. |

`deploy.sh` v1.0 phải implement Option A rõ ràng. Không còn mô tả “build local -> scp binary” trong cùng design vì gây mâu thuẫn.

---

## 14. Validation Checklist trước Production

- `aws sts get-caller-identity` pass với `infra/aws.env.local`.
- `provision.sh` tạo EC2/EIP/SG/key pair và ghi `.ec2-state`.
- Security Group inbound chỉ có SSH từ `MY_IP/32`.
- Security Group egress khớp mode đã chọn trong section 4.
- Elastic IP đã whitelist trong Binance API Management.
- `deploy.sh` reject `infra/prod.env` nếu chứa `AWS_`.
- `/opt/binance-bot/prod.env` có permission `600`.
- `systemctl status binance-bot` running.
- `journalctl -u binance-bot` không log secret.
- Signed endpoint smoke test pass sau whitelist.
- `teardown.sh` được test trong môi trường non-production hoặc dry-run bằng resource tags.

---

## 15. References

- AWS VPC pricing, public IPv4 charge: <https://aws.amazon.com/vpc/pricing/>
- AWS EC2 T3 instance behavior and CPU credits: <https://aws.amazon.com/ec2/instance-types/t3/>
- Binance USD-M Futures general info: <https://developers.binance.com/docs/derivatives/usds-margined-futures/general-info>
- Binance USD-M Futures WebSocket market streams: <https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams>
