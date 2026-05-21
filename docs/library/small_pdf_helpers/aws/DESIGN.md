# AWS PDF → Markdown Converter — Design Document

**Date:** 2026-03-18
**Status:** Approved

---

## Understanding Summary

- **What:** Thay thế backend `convert_pdf_to_markdown()` local (marker-pdf + GPU) bằng SageMaker Processing Job
- **Why:** Máy local thiếu GPU đủ mạnh; muốn dùng được trên nhiều máy không cần cài models (~GB)
- **Who:** Developer dùng nội bộ, tần suất thấp (vài file/ngày)
- **Constraints:** Tần suất thấp → zero idle cost là bắt buộc; nội dung PDF không nhạy cảm
- **Non-goals:** Không xử lý batch, không web interface, tab Cắt PDF giữ nguyên local

---

## Assumptions

- File PDF kích thước thông thường (< 100MB)
- Chấp nhận cold start 3–5 phút mỗi job
- AWS credentials cấu hình qua `aws configure` trên máy local
- Chi phí ~$2/tháng với 10 jobs là chấp nhận được
- Giữ local fallback — AWS là opt-in, không thay thế hoàn toàn

---

## Architecture

```
[Local Machine]                    [AWS]
─────────────────                  ─────────────────────────────────
Tkinter App                        S3 Bucket: pdf-converter-jobs/
  │                                  ├── input/{job_id}.pdf
  ├─ upload PDF ──────────────────►  └── output/{job_id}.md
  │
  ├─ submit job ──────────────────► SageMaker Processing Job
  │                                    └── Container (ECR)
  ├─ poll status (boto3) ◄──────────        ├── marker-pdf (baked)
  │                                         └── convert.py
  └─ download .md ◄───────────────  S3 output/{job_id}.md
```

### Luồng chính

1. User chọn PDF → bấm "Chuyển sang MD (AWS)"
2. App tạo `job_id = uuid4()`, upload PDF lên `s3://bucket/input/{job_id}.pdf`
3. App submit SageMaker Processing Job trỏ vào S3 input/output
4. App poll status mỗi 15 giây — UI hiển thị trạng thái text
5. Job hoàn thành → App download `output/{job_id}.md` về local output dir
6. Cleanup: xóa cả hai file tạm trên S3
7. Thông báo thành công

---

## Components

### ✅ DONE 1. Docker Container (ECR)

**File layout:**
```
small_pdf_helpers/aws/
├── Dockerfile
├── convert.py
└── DESIGN.md
```

**Dockerfile:**
```dockerfile
FROM python:3.12-slim

RUN pip install marker-pdf

# Pre-bake models vào image — tránh download lại mỗi job
RUN python -c "from marker.models import create_model_dict; create_model_dict()"

COPY convert.py /opt/convert.py
ENTRYPOINT ["python", "/opt/convert.py"]
```

**convert.py** đọc từ `SM_CHANNEL_INPUT` / `SM_CHANNEL_OUTPUT` (SageMaker env vars):
```
/opt/ml/processing/input/  → PDF file
/opt/ml/processing/output/ → MD file output
```

### ✅ DONE 2. Local AWS Integration

**File mới:** `src/utils/pdf_tools_aws.py`

```python
def convert_pdf_to_markdown_aws(
    input_pdf: str,
    output_dir: str,
    progress_callback: ProgressCallback | None = None,
) -> str:
    # 1. Upload PDF lên S3
    # 2. Submit SageMaker Processing Job
    # 3. Poll mỗi 15s với progress_callback
    # 4. Download .md về output_dir
    # 5. Cleanup S3 (finally block)
```

**Config** (đọc từ `aws_config.json`, gitignored):
```json
{
  "aws_region": "ap-southeast-1",
  "s3_bucket": "pdf-converter-jobs",
  "ecr_image_uri": "xxxx.dkr.ecr.ap-southeast-1.amazonaws.com/pdf-converter:latest",
  "sagemaker_role_arn": "arn:aws:iam::xxxx:role/SageMakerRole",
  "instance_type": "ml.g4dn.xlarge"
}
```

**Poll strategy:**
```
submit job
  └── loop mỗi 15s:
        ├── "InProgress" → callback("Đang xử lý... (Xm Xs)")
        ├── "Completed"  → download → return path
        ├── "Failed"     → raise RuntimeError(failure_reason)
        └── timeout 30 phút → stop_processing_job() + raise TimeoutError
```

### ✅ DONE 3. UI Changes (main.py)

- Progress bar indeterminate → progress text label với các trạng thái:
  - `"Đang tải file lên S3..."`
  - `"Đang khởi động server (ước tính 3–5 phút)..."`
  - `"Đang chuyển đổi... (Xm Xs)"`
  - `"Đang tải kết quả về..."`
- Cảnh báo khi đóng app lúc job đang chạy → offer hủy job

---

## Error Handling

| Tình huống | Xử lý |
|---|---|
| AWS credentials chưa cấu hình | Bắt `NoCredentialsError` → hướng dẫn chạy `aws configure` |
| S3 bucket không tồn tại | Bắt `NoSuchBucket` → thông báo tên bucket cần tạo |
| Job bị từ chối (quota) | Bắt `ResourceLimitExceeded` → thông báo tăng quota |
| Job failed | Lấy `failure_reason` từ SageMaker, hiển thị cho user |
| Job timeout > 30 phút | Hủy job, thông báo timeout |
| User đóng app khi job chạy | Hỏi có muốn hủy job không, gọi `stop_processing_job()` |
| Output rỗng | Cảnh báo "Không trích xuất được text" |
| `aws_config.json` thiếu | Thông báo hướng dẫn tạo config file |

---

## One-time AWS Setup

```
1. aws configure
   └── Access Key, Secret Key, region: ap-southeast-1

2. S3: tạo bucket pdf-converter-jobs
   └── Block all public access

3. IAM Role: SageMakerRole
   └── Trust: sagemaker.amazonaws.com
   └── Policies: AmazonS3FullAccess, AmazonSageMakerFullAccess

4. ECR: tạo repo pdf-converter
   └── Build & push Docker image

5. Tạo aws_config.json (gitignored)
```

---

## Cost Estimate

| Item | Chi phí |
|---|---|
| ECR storage ~10GB | ~$1/tháng |
| S3 temp files | < $0.01/tháng |
| ml.g4dn.xlarge ~10 phút/job | ~$0.12/job |
| **10 jobs/tháng** | **~$2.2/tháng** |

---

## Decision Log

| Quyết định | Lý do | Alternatives loại bỏ |
|---|---|---|
| SageMaker Processing Job | Pay-per-use, zero idle cost | Async Endpoint (phức tạp hơn), EC2 Spot (rủi ro bị thu hồi) |
| Models baked vào Docker image | Startup nhanh hơn, tránh download lặp | Download at runtime (chậm hơn 5 phút/job) |
| Region ap-southeast-1 | Gần VN nhất, latency thấp | us-east-1 (xa hơn) |
| ml.g4dn.xlarge | GPU nhỏ nhất đủ cho marker-pdf | ml.p3.2xlarge (đắt hơn 3x) |
| Giữ local fallback | Dùng được khi không có internet/AWS | Xóa hẳn local mode |
| aws_config.json gitignored | Không leak credentials/ARN lên git | Hardcode (nguy hiểm) |

