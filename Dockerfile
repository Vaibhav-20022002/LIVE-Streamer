ARG BUILD_TYPE=release
ARG ORT_VERSION=1.20.1

FROM ubuntu:24.04 AS builder
ARG BUILD_TYPE
ARG ORT_VERSION
ARG TARGETARCH

RUN DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    meson \
    ninja-build \
    pkg-config \
    curl \
    unzip \
    ca-certificates \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libavdevice-dev \
    libswscale-dev \
    libx264-dev \
    && rm -rf /var/lib/apt/lists/*

# Map TARGETARCH to NuGet Linux RID
RUN case "${TARGETARCH:-amd64}" in \
        amd64) echo "linux-x64" > /tmp/ort_rid ;; \
        arm64) echo "linux-arm64" > /tmp/ort_rid ;; \
        *) echo "linux-x64" > /tmp/ort_rid ;; \
    esac

RUN ORT_RID=$(cat /tmp/ort_rid) && \
    curl -fsSL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${ORT_VERSION}/microsoft.ml.onnxruntime.${ORT_VERSION}.nupkg" -o /tmp/ort.nupkg && \
    unzip -q /tmp/ort.nupkg "build/native/include/*" "runtimes/${ORT_RID}/native/*" -d /tmp/ort_extract && \
    mkdir -p /opt/onnxruntime/include /opt/onnxruntime/lib && \
    cp -r /tmp/ort_extract/build/native/include/* /opt/onnxruntime/include/ && \
    cp -a /tmp/ort_extract/runtimes/${ORT_RID}/native/libonnxruntime.so* /opt/onnxruntime/lib/ && \
    if [ -f /opt/onnxruntime/lib/libonnxruntime.so ] && [ ! -e "/opt/onnxruntime/lib/libonnxruntime.so.${ORT_VERSION}" ]; then \
        ln -sf libonnxruntime.so "/opt/onnxruntime/lib/libonnxruntime.so.${ORT_VERSION}"; \
    fi && \
    rm -rf /tmp/ort.nupkg /tmp/ort_extract /tmp/ort_rid

# Bundle default COCO-80 YOLOv8n ONNX model so fresh clones run AI inference out of the box
RUN mkdir -p /opt/models && \
    (curl -fsSL "https://huggingface.co/Kalray/yolov8/resolve/main/yolov8n.onnx" -o /opt/models/yolov8n.onnx || \
     curl -fsSL "https://huggingface.co/unileon-robotics/YOLO8-ONNX/resolve/main/yolov8n.onnx" -o /opt/models/yolov8n.onnx) && \
    test -s /opt/models/yolov8n.onnx

COPY . /src
WORKDIR /src

RUN meson setup build --buildtype=${BUILD_TYPE} -Denable_ai=enabled -Donnxruntime_dir=/opt/onnxruntime && \
    meson compile -C build && \
    meson test -C build --print-errorlogs

FROM ubuntu:24.04 AS runtime

RUN DEBIAN_FRONTEND=noninteractive apt-get update && apt-get install -y --no-install-recommends \
    libavformat60 \
    libavcodec60 \
    libavutil58 \
    libavdevice60 \
    libswscale7 \
    libx264-164 \
    libstdc++6 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/onnxruntime/lib /opt/onnxruntime/lib
COPY --from=builder /opt/models/yolov8n.onnx /opt/models/yolov8n.onnx
COPY --from=builder /src/build/live_streamer /app/live_streamer

RUN echo "/opt/onnxruntime/lib" > /etc/ld.so.conf.d/onnxruntime.conf && ldconfig

ENV LD_LIBRARY_PATH=/opt/onnxruntime/lib \
    HLS_OUTPUT_DIR=/data/hls \
    AI_MODEL_PATH=/opt/models/yolov8n.onnx

RUN mkdir -p /data/hls /app/models

WORKDIR /app
ENTRYPOINT ["/app/live_streamer"]
