#!/usr/bin/env python3
import argparse
import os
import sys

def main():
    parser = argparse.ArgumentParser(description="Export YOLOv8 model to ONNX format for live streamer.")
    parser.add_argument("--imgsz", type=int, default=320, help="Inference image size (default: 320)")
    parser.add_argument("--output", type=str, default="models/yolov8n_320.onnx", help="Output ONNX file path")
    args = parser.parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        print("Error: 'ultralytics', 'onnx', or 'onnxslim' packages are not installed.", file=sys.stderr)
        print("Please install them via: pip install ultralytics onnx onnxslim", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    print(f"Loading YOLOv8n and exporting to ONNX with imgsz={args.imgsz}...")
    model = YOLO("yolov8n.pt")
    
    # Export returns the path to the exported file
    exported_path = model.export(format="onnx", imgsz=args.imgsz, opset=12, simplify=True)

    # Move/copy exported model to requested output path if different
    if os.path.abspath(exported_path) != os.path.abspath(args.output):
        import shutil
        shutil.move(exported_path, args.output)

    print(f"Successfully exported and saved model to: {args.output}")

if __name__ == "__main__":
    main()
