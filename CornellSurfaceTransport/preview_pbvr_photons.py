"""Compare staged/reference, conventional, first PBVR, and ensemble-mean images.

Optional Pillow dependency; no role in simulation or error measurements.
"""
import argparse
import csv
from pathlib import Path
from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--indirect", action="store_true")
    args = parser.parse_args()
    with (args.results / "pbvr_metrics.csv").open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    pbvr = [row for row in rows if row["mode"] == "pbvr"]
    count = pbvr[-1]["ensembles"]
    path = next(row for row in rows if row["mode"] == "path")
    panels = [
        ("reference", "Staged / no thinning", rows[0]),
        ("path", "Conventional roulette", path),
        ("pbvr_first", "PBVR / 1 ensemble", pbvr[0]),
        ("pbvr_mean", f"PBVR / {count} ensembles", pbvr[-1]),
    ]
    edge, title = 320, 64
    canvas = Image.new("RGB", (edge * 4, edge + title), "#202020")
    draw = ImageDraw.Draw(canvas)
    for i, (name, label, metrics) in enumerate(panels):
        error = float(metrics["indirect_relative_l2"]) * 100
        draw.text((i * edge + 8, 7), label, fill="white")
        draw.text((i * edge + 8, 24), f"Indirect relative L2: {error:.2f}%", fill="white")
        draw.text((i * edge + 8, 41), "Indirect only" if args.indirect else "Emission + direct + indirect", fill="#b0b0b0")
        suffix = "_indirect" if args.indirect else ""
        with Image.open(args.results / (name + suffix + ".ppm")) as image:
            canvas.paste(image.convert("RGB").resize((edge, edge), Image.Resampling.NEAREST), (i * edge, title))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(args.output)


if __name__ == "__main__":
    main()
