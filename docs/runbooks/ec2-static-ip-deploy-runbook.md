# EC2 Static IP Deploy Runbook

## 1. Scope

Runbook này dùng để:

- Provision EC2 + Elastic IP cho Binance whitelist IP.
- Deploy bot lên EC2 bằng build-on-EC2 workflow.
- Vận hành kiểm tra trạng thái service.
- Teardown tài nguyên khi không cần dùng.

## 2. Preconditions

- Có AWS account + quyền EC2, EIP, SG, key pair.
- Cài sẵn `aws`, `ssh`, `scp`, `rsync`, `cmake`.
- File secrets local:
  - `infra/aws.env.local` (AWS credentials local-only)
  - `infra/prod.env` (Binance runtime secrets)

Templates:

- `infra/aws.env.local.example`
- `infra/prod.env.example`

## 3. Quick Start (Agent-safe)

```bash
./docs/runbooks/scripts/00-preflight.sh
./docs/runbooks/scripts/01-aws-auth-check.sh
./docs/runbooks/scripts/02-provision.sh
./docs/runbooks/scripts/03-ec2-status.sh
```

Sau khi provision xong, lấy `Elastic IP` và whitelist trong Binance API Management.

Tiếp theo:

```bash
./docs/runbooks/scripts/04-deploy.sh
./docs/runbooks/scripts/05-service-status.sh
```

Xem log realtime:

```bash
./docs/runbooks/scripts/06-tail-logs.sh
```

## 4. Repeated Tasks

### 4.1 IP local đổi, không SSH được

```bash
./infra/update-ssh-ip.sh
```

### 4.2 Kiểm tra EC2 còn online không

```bash
./docs/runbooks/scripts/03-ec2-status.sh
```

### 4.3 Re-deploy code mới

```bash
./docs/runbooks/scripts/04-deploy.sh
```

### 4.4 Shutdown toàn bộ để dừng chi phí

```bash
./docs/runbooks/scripts/07-teardown.sh
```

## 5. Troubleshooting

### 5.1 `AWS auth failed`

- Kiểm tra file `infra/aws.env.local`.
- Chạy lại `./docs/runbooks/scripts/01-aws-auth-check.sh`.
- Nếu dùng session token, đảm bảo token chưa hết hạn.

### 5.2 `infra/prod.env contains AWS_*`

- Xóa toàn bộ biến `AWS_` khỏi `infra/prod.env`.
- `infra/prod.env` chỉ giữ `BINANCE_API_KEY`, `BINANCE_SECRET_KEY`.

### 5.3 SSH timeout sau provision

- Chạy `./infra/update-ssh-ip.sh`.
- Kiểm tra SG inbound chỉ mở TCP 22 cho IP hiện tại.

### 5.4 Service `binance-bot` failed

- Chạy `./docs/runbooks/scripts/05-service-status.sh`.
- Chạy `./docs/runbooks/scripts/06-tail-logs.sh`.
- Fix code hoặc env rồi re-deploy.

## 6. Notes For Agents

- Không commit `infra/aws.env.local`, `infra/prod.env`, `.pem`, `infra/.ec2-state`.
- Không dùng root `.env` để deploy production.
- Luôn cung cấp Elastic IP hiện tại cho operator trước khi chạy production nếu IP thay đổi.
