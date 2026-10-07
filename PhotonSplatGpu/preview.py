"""Optional Pillow preview: KDTree, GPU splat, magnified linear indirect error."""
import argparse
import array
from pathlib import Path
import sys
from PIL import Image, ImageDraw


def read_pfm(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError("Expected RGB PFM")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        values = array.array("f")
        values.frombytes(stream.read())
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    if len(values) != width * height * 3:
        raise ValueError("Wrong PFM pixel count")
    return width, height, values


parser = argparse.ArgumentParser()
parser.add_argument("results", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
width, height, cpu = read_pfm(args.results / "kdtree_indirect.pfm")
gpu_width, gpu_height, gpu = read_pfm(args.results / "splat_indirect.pfm")
if (width, height) != (gpu_width, gpu_height):
    raise ValueError("Image dimensions differ")
heatmap = Image.new("RGB", (width, height))
pixels = heatmap.load()
for y in range(height):
    for x in range(width):
        offset = ((height - 1 - y) * width + x) * 3
        pixels[x, y] = tuple(round(min(1, abs(cpu[offset + c] - gpu[offset + c]) * 1000) * 255) for c in range(3))
canvas = Image.new("RGB", (1152, 412), "#202020")
draw = ImageDraw.Draw(canvas)
for i, (name, label) in enumerate([
    ("kdtree", "KDTree / same G-buffer"),
    ("splat", "GPU additive photon splats"),
    (None, "Absolute indirect error x1000"),
]):
    if name:
        with Image.open(args.results / (name + ".ppm")) as image:
            display = image.convert("RGB").resize((384, 384))
    else:
        display = heatmap.resize((384, 384), Image.Resampling.NEAREST)
    canvas.paste(display, (i * 384, 28))
    draw.text((i * 384 + 8, 8), label, fill="white")
args.output.parent.mkdir(parents=True, exist_ok=True)
canvas.save(args.output)
