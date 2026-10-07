"""Create a labeled preview from PPM outputs (optional dependency: Pillow)."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument("results", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
canvas = Image.new("RGB", (1152, 412), "#202020")
draw = ImageDraw.Draw(canvas)
for index, (name, label) in enumerate([
    ("direct", "Direct only"),
    ("surface", "Surface particles / reduced higher orders"),
    ("reference", "Path tracing reference"),
]):
    with Image.open(args.results / (name + ".ppm")) as source:
        canvas.paste(source.convert("RGB").resize((384, 384)), (index * 384, 28))
    draw.text((index * 384 + 8, 8), label, fill="white")
args.output.parent.mkdir(parents=True, exist_ok=True)
canvas.save(args.output)
