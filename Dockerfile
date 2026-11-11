FROM ubuntu:26.04
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-pil whisper.cpp opencc \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY bridge/ ./bridge/
ENV PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["python3", "/app/bridge/server.py"]
