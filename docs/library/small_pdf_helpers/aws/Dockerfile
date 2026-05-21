FROM pytorch/pytorch:2.1.0-cuda11.8-cudnn8-runtime

RUN pip install --no-cache-dir marker-pdf

# Pre-bake marker models to avoid runtime downloads on every job.
RUN python -c "from marker.models import create_model_dict; create_model_dict()"

COPY convert.py /opt/convert.py
ENTRYPOINT ["python", "/opt/convert.py"]
