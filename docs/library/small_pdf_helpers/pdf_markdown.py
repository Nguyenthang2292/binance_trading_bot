try:
    from .utils.pdf_tools import (
        configure_marker_env,
        convert_pdf_to_markdown,
        resolve_markdown_output_path,
    )
    from .utils.pdf_tools_aws import (
        AwsJobCancelledError,
        convert_pdf_to_markdown_aws,
        load_aws_pdf_config,
    )
    from .utils.pdf_tools_unstructured import (
        convert_pdf_to_markdown as convert_pdf_to_markdown_unstructured,
    )
except ImportError:
    from utils.pdf_tools import (
        configure_marker_env,
        convert_pdf_to_markdown,
        resolve_markdown_output_path,
    )
    from utils.pdf_tools_aws import (
        AwsJobCancelledError,
        convert_pdf_to_markdown_aws,
        load_aws_pdf_config,
    )
    from utils.pdf_tools_unstructured import (
        convert_pdf_to_markdown as convert_pdf_to_markdown_unstructured,
    )

__all__ = [
    "AwsJobCancelledError",
    "configure_marker_env",
    "convert_pdf_to_markdown",
    "convert_pdf_to_markdown_aws",
    "convert_pdf_to_markdown_unstructured",
    "load_aws_pdf_config",
    "resolve_markdown_output_path",
]

