from __future__ import annotations

import json
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from threading import Event
from typing import Callable

import boto3
from botocore.exceptions import ClientError, NoCredentialsError, PartialCredentialsError

try:
    from .pdf_gemini_repair_runner import repair_with_gemini as _repair_with_gemini
    from .pdf_tools import resolve_markdown_output_path
except ImportError:
    from pdf_gemini_repair_runner import repair_with_gemini as _repair_with_gemini
    from pdf_tools import resolve_markdown_output_path

AwsProgressCallback = Callable[[str], None]

_POLL_INTERVAL_SECONDS = 15
_DEFAULT_TIMEOUT_SECONDS = 30 * 60
_CW_LOG_GROUP = "/aws/sagemaker/ProcessingJobs"


class AwsJobCancelledError(RuntimeError):
    """Raised when user requests to cancel an in-flight SageMaker job."""


@dataclass(frozen=True)
class AwsPdfConfig:
    aws_region: str
    s3_bucket: str
    ecr_image_uri: str
    sagemaker_role_arn: str
    instance_type: str


def _default_config_path() -> Path:
    module_root = Path(__file__).resolve().parents[1]
    return module_root / "aws" / "aws_config.json"


def _emit_status(progress_callback: AwsProgressCallback | None, message: str) -> None:
    if progress_callback is None:
        return
    try:
        progress_callback(message)
    except Exception:
        return


def _safe_delete_s3_object(s3_client: object, bucket: str, key: str) -> None:
    try:
        s3_client.delete_object(Bucket=bucket, Key=key)  # type: ignore[union-attr]
    except Exception:
        return


def _safe_stop_processing_job(sm_client: object, job_name: str) -> None:
    try:
        sm_client.stop_processing_job(ProcessingJobName=job_name)  # type: ignore[union-attr]
    except Exception:
        return


def _format_elapsed(seconds: int) -> str:
    minutes = seconds // 60
    remainder = seconds % 60
    return f"{minutes}m {remainder}s"


class _CwLogTailer:
    """Pull new CloudWatch log events for a SageMaker Processing Job each poll cycle."""

    def __init__(self, logs_client: object, job_name: str) -> None:
        self._client = logs_client
        self._job_name = job_name
        self._stream_name: str | None = None
        self._next_token: str | None = None

    def _find_stream(self) -> str | None:
        try:
            resp = self._client.describe_log_streams(  # type: ignore[union-attr]
                logGroupName=_CW_LOG_GROUP,
                logStreamNamePrefix=self._job_name,
                limit=1,
            )
            streams = resp.get("logStreams", [])
            if streams:
                return streams[0]["logStreamName"]
        except Exception:
            pass
        return None

    def drain(self) -> list[str]:
        """Return new log lines since last call (empty list when nothing new)."""
        if self._stream_name is None:
            self._stream_name = self._find_stream()
        if self._stream_name is None:
            return []

        messages: list[str] = []
        try:
            kwargs: dict[str, object] = {
                "logGroupName": _CW_LOG_GROUP,
                "logStreamName": self._stream_name,
                "startFromHead": True,
            }
            if self._next_token is not None:
                kwargs["nextToken"] = self._next_token

            resp = self._client.get_log_events(**kwargs)  # type: ignore[union-attr]
            for event in resp.get("events", []):
                msg = event.get("message", "").rstrip()
                if msg:
                    messages.append(f"[container] {msg}")
            self._next_token = resp.get("nextForwardToken")
        except Exception:
            pass
        return messages


def _find_output_key(job_id: str) -> str:
    # PDF được upload lên S3 với tên {job_id}.pdf nên container tạo output {job_id}.md
    return f"output/{job_id}/{job_id}.md"


def load_aws_pdf_config(config_path: str | None = None) -> AwsPdfConfig:
    path = Path(config_path).expanduser().resolve() if config_path else _default_config_path()
    if not path.is_file():
        raise FileNotFoundError(
            f"Không tìm thấy file cấu hình AWS: {path}. "
            "Vui lòng tạo file aws_config.json theo hướng dẫn."
        )

    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"File cấu hình AWS không hợp lệ JSON: {path}") from error

    required_keys = [
        "aws_region",
        "s3_bucket",
        "ecr_image_uri",
        "sagemaker_role_arn",
        "instance_type",
    ]
    missing = [key for key in required_keys if not raw.get(key)]
    if missing:
        raise ValueError(f"Thiếu cấu hình bắt buộc trong aws_config.json: {', '.join(missing)}")

    return AwsPdfConfig(
        aws_region=str(raw["aws_region"]),
        s3_bucket=str(raw["s3_bucket"]),
        ecr_image_uri=str(raw["ecr_image_uri"]),
        sagemaker_role_arn=str(raw["sagemaker_role_arn"]),
        instance_type=str(raw["instance_type"]),
    )


def convert_pdf_to_markdown_aws(
    input_pdf: str,
    output_dir: str,
    progress_callback: AwsProgressCallback | None = None,
    cancel_event: Event | None = None,
    config_path: str | None = None,
    timeout_seconds: int = _DEFAULT_TIMEOUT_SECONDS,
) -> str:
    input_path = Path(input_pdf).expanduser().resolve()
    output_directory = Path(output_dir).expanduser().resolve()

    if not input_path.is_file():
        raise ValueError("File PDF nguồn không tồn tại.")
    if not output_directory.is_dir():
        raise ValueError("Thư mục lưu không tồn tại.")

    config = load_aws_pdf_config(config_path=config_path)
    output_path = resolve_markdown_output_path(str(output_directory), str(input_path))

    s3_client = boto3.client("s3", region_name=config.aws_region)
    sm_client = boto3.client("sagemaker", region_name=config.aws_region)
    logs_client = boto3.client("logs", region_name=config.aws_region)

    job_id = uuid.uuid4().hex
    job_name = f"pdf-md-{job_id}"[:63]
    input_key = f"input/{job_id}.pdf"
    output_key: str | None = None

    _emit_status(progress_callback, "Đang tải file lên S3...")
    try:
        s3_client.upload_file(str(input_path), config.s3_bucket, input_key)

        _emit_status(progress_callback, "Đang khởi động server (ước tính 3-5 phút)...")
        sm_client.create_processing_job(
            ProcessingJobName=job_name,
            RoleArn=config.sagemaker_role_arn,
            AppSpecification={"ImageUri": config.ecr_image_uri},
            ProcessingResources={
                "ClusterConfig": {
                    "InstanceCount": 1,
                    "InstanceType": config.instance_type,
                    "VolumeSizeInGB": 30,
                }
            },
            ProcessingInputs=[
                {
                    "InputName": "input",
                    "S3Input": {
                        "S3Uri": f"s3://{config.s3_bucket}/{input_key}",
                        "LocalPath": "/opt/ml/processing/input",
                        "S3DataType": "S3Prefix",
                        "S3InputMode": "File",
                        "S3DataDistributionType": "FullyReplicated",
                    },
                }
            ],
            ProcessingOutputConfig={
                "Outputs": [
                    {
                        "OutputName": "output",
                        "S3Output": {
                            "S3Uri": f"s3://{config.s3_bucket}/output/{job_id}/",
                            "LocalPath": "/opt/ml/processing/output",
                            "S3UploadMode": "EndOfJob",
                        },
                    }
                ]
            },
            StoppingCondition={"MaxRuntimeInSeconds": timeout_seconds},
        )

        log_tailer = _CwLogTailer(logs_client, job_name)
        start_time = time.monotonic()
        while True:
            if cancel_event is not None and cancel_event.is_set():
                _safe_stop_processing_job(sm_client, job_name)
                raise AwsJobCancelledError("Đã hủy job AWS theo yêu cầu người dùng.")

            elapsed_seconds = int(time.monotonic() - start_time)
            if elapsed_seconds > timeout_seconds:
                _safe_stop_processing_job(sm_client, job_name)
                raise TimeoutError("Job AWS vượt quá thời gian chờ 30 phút.")

            # Stream CloudWatch logs to terminal before checking job status
            for log_line in log_tailer.drain():
                _emit_status(progress_callback, log_line)

            status_response = sm_client.describe_processing_job(ProcessingJobName=job_name)
            status = status_response.get("ProcessingJobStatus", "")

            if status == "Completed":
                # Drain any remaining logs before downloading result
                for log_line in log_tailer.drain():
                    _emit_status(progress_callback, log_line)

                _emit_status(progress_callback, "Đang tải kết quả về...")
                output_key = _find_output_key(job_id)
                s3_client.download_file(config.s3_bucket, output_key, str(output_path))

                if not output_path.read_text(encoding="utf-8").strip():
                    raise RuntimeError("Không trích xuất được text từ PDF.")
                _emit_status(progress_callback, "Đang fix bảng quẻ bằng Gemini...")
                _repair_with_gemini(str(input_path), output_path, progress_callback)
                return str(output_path)

            if status in {"Failed", "Stopped"}:
                for log_line in log_tailer.drain():
                    _emit_status(progress_callback, log_line)
                failure_reason = status_response.get("FailureReason") or "Không rõ nguyên nhân."
                raise RuntimeError(f"SageMaker job thất bại: {failure_reason}")

            _emit_status(progress_callback, f"Đang chuyển đổi... ({_format_elapsed(elapsed_seconds)})")
            time.sleep(_POLL_INTERVAL_SECONDS)

    except (NoCredentialsError, PartialCredentialsError) as error:
        raise RuntimeError("AWS credentials chưa cấu hình. Vui lòng chạy `aws configure`.") from error
    except ClientError as error:
        code = error.response.get("Error", {}).get("Code", "")
        if code == "NoSuchBucket":
            raise RuntimeError(
                f"S3 bucket `{config.s3_bucket}` không tồn tại. Hãy tạo bucket trước khi chạy."
            ) from error
        if code == "ResourceLimitExceeded":
            aws_message = error.response.get("Error", {}).get("Message", "")
            raise RuntimeError(
                f"SageMaker quota không đủ (ResourceLimitExceeded).\nChi tiết AWS: {aws_message}"
            ) from error
        raise RuntimeError(f"AWS ClientError ({code or 'unknown'}): {error}") from error
    finally:
        _safe_delete_s3_object(s3_client, config.s3_bucket, input_key)
        if output_key:
            _safe_delete_s3_object(s3_client, config.s3_bucket, output_key)


__all__ = [
    "AwsJobCancelledError",
    "AwsPdfConfig",
    "AwsProgressCallback",
    "convert_pdf_to_markdown_aws",
    "load_aws_pdf_config",
]

