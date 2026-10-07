"""Compare index/storage bypass and PBVR power/spatial resampling at ~5% error.

Reuses the independent 32M-photon reference, never its timing as a baseline.
Standard library for measurements; Pillow only for report previews.
"""
import argparse
import csv
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys
sys.dont_write_bytecode = True
import run_photon_quality as common

RESULTS = common.ROOT / "scratch/photon-optimized-quality"
REPORT = common.ROOT / "reaserch/pbvr_photon_transport/results/2026-10-07/optimized_quality"
METHODS = {
    # mode, retention, cap fraction, selection, cell size, index, store direct
    "path_indexed": ("path", 1, 0, "uniform", 0, 1, 1),
    "path_flat": ("path", 1, 0, "uniform", 0, 0, 0),
    "uniform_80": ("pbvr", 0.8, 0, "uniform", 0, 0, 0),
    "uniform_capped": ("pbvr", 0.65, 0.2, "uniform", 0, 0, 0),
    "power_65": ("pbvr", 0.65, 0, "power", 0, 0, 0),
    "power_80": ("pbvr", 0.8, 0, "power", 0, 0, 0),
    "power_capped": ("pbvr", 0.65, 0.2, "power", 0, 0, 0),
    "spatial_65": ("pbvr", 0.65, 0, "spatial", 0.12, 0, 0),
    "spatial_80": ("pbvr", 0.8, 0, "spatial", 0.12, 0, 0),
    "spatial_capped": ("pbvr", 0.65, 0.2, "spatial", 0.12, 0, 0),
    "spatial_coarse": ("pbvr", 0.65, 0.2, "spatial", 0.24, 0, 0),
}


def render(method, count, seed, phase):
    mode, retention, fraction, selection, cell, index, direct = METHODS[method]
    destination = RESULTS / f"{phase}-{method}-n{count}-seed{seed}"
    metrics = destination / "quality_metrics.csv"
    if not metrics.exists():
        command = [str(common.EXE), "--photon-quality", str(destination), "96", str(count), "0.12", "32",
                   str(seed), mode, str(retention), str(round(count * fraction)), "1", selection, str(cell), str(index), str(direct)]
        print(f"Running {destination.name}", flush=True)
        subprocess.run(command, check=True)
    with metrics.open(newline="", encoding="utf-8-sig") as stream:
        row = next(csv.DictReader(stream))
    expected = {"mode": mode, "photon_count": str(count), "seed": str(seed), "ensembles": "1", "selection": selection,
                "build_index": str(index), "store_direct": str(direct), "cap": str(round(count * fraction))}
    if any(row[k] != v for k, v in expected.items()) or float(row["retention"]) != retention or float(row["cell_size"]) != cell:
        raise ValueError(f"Cached parameters differ: {destination}; use a new --results directory")
    row["directory"] = str(destination); row["method"] = method; row["phase"] = phase
    return row


def group(method, count, ref, phase, seeds=(42, 43, 44)):
    rows = [common.evaluate(render(method, count, seed, phase), ref) for seed in seeds]
    rms = math.sqrt(statistics.mean(float(r["relative_l2"]) ** 2 for r in rows))
    duration = statistics.median(float(r["render_total_seconds"]) for r in rows)
    print(f"{method} N={count}: RMS L2={rms:.4%}, median={duration:.6f}s", flush=True)
    return rows, rms, duration


def pilot():
    ref = common.reference_image()
    rows = []
    for method in METHODS:
        trials, _, _ = group(method, 50000, ref, "pilot")
        rows.extend(trials)
    common.write_csv(RESULTS / "pilot.csv", rows)


def match():
    ref = common.reference_image()
    with (RESULTS / "pilot.csv").open(newline="") as stream:
        pilots = list(csv.DictReader(stream))
    scores = {}
    for method in METHODS:
        rows = [r for r in pilots if r["method"] == method]
        scores[method] = statistics.mean(float(r["relative_l2"]) ** 2 for r in rows) * statistics.median(float(r["indirect_total_seconds"]) for r in rows)
    power = min((m for m in METHODS if m.startswith("power_")), key=scores.get)
    spatial = min((m for m in METHODS if m.startswith("spatial_")), key=scores.get)
    print(f"Pilot efficiency scores={scores}; selected power={power}, spatial={spatial}", flush=True)
    all_trials, matches = [], []
    goal = 0.05
    for method in ["path_indexed", "path_flat", "uniform_80", "uniform_capped", power, spatial]:
        rows = [r for r in pilots if r["method"] == method]
        mse = statistics.mean(float(r["relative_l2"]) ** 2 for r in rows)
        count = max(10000, round(50000 * mse / (goal * goal) / 1000) * 1000)
        for attempt in range(3):
            tuning, rms, _ = group(method, count, ref, "tune", (142, 143, 144))
            all_trials.extend(tuning)
            if abs(rms / goal - 1) <= 0.03:
                break
            revised = max(10000, round(count * (rms / goal) ** 2 / 1000) * 1000)
            if revised == count:
                break
            count = revised
        trials, rms, median = group(method, count, ref, "match")
        all_trials.extend(trials)
        matches.append({"method": method, "photons": count, "rms_relative_l2": rms, "median_render_seconds": median,
                        "median_indirect_seconds": statistics.median(float(r["indirect_total_seconds"]) for r in trials),
                        "median_transport_seconds": statistics.median(float(r["transport_seconds"]) for r in trials),
                        "median_map_seconds": statistics.median(float(r["map_seconds"]) for r in trials),
                        "median_rays": statistics.median(int(r["traced_rays"]) for r in trials),
                        "mean_ratio": statistics.mean(float(r["mean_ratio"]) for r in trials)})
    common.write_csv(RESULTS / "trials.csv", all_trials)
    common.write_csv(RESULTS / "matched.csv", matches)


def timing():
    ref = common.reference_image()
    with (RESULTS / "matched.csv").open(newline="") as stream:
        matches = list(csv.DictReader(stream))
    with (RESULTS / "trials.csv").open(newline="") as stream:
        trials = list(csv.DictReader(stream))
    jobs = [(repeat, row, seed) for repeat in [1, 2] for row in matches for seed in [42, 43, 44]]
    random.Random(271828).shuffle(jobs); repeat_rows = []
    for repeat, row, seed in jobs:
        rows, _, _ = group(row["method"], int(row["photons"]), ref, f"timing{repeat}", (seed,))
        repeat_rows.extend(rows)
    common.write_csv(RESULTS / "timing_trials.csv", repeat_rows)
    for row in matches:
        rows = [r for r in trials if r["method"] == row["method"] and r["phase"] == "match"]
        rows += [r for r in repeat_rows if r["method"] == row["method"]]
        for name in ["render", "indirect", "transport", "map"]:
            field = "render_total_seconds" if name == "render" else "indirect_total_seconds" if name == "indirect" else name + "_seconds"
            row[f"median_{name}_seconds"] = statistics.median(float(r[field]) for r in rows)
        row["min_render_seconds"] = min(float(r["render_total_seconds"]) for r in rows)
        row["max_render_seconds"] = max(float(r["render_total_seconds"]) for r in rows)
    common.write_csv(RESULTS / "matched.csv", matches)


def report():
    from PIL import Image, ImageDraw
    ref = common.reference_image(); REPORT.mkdir(parents=True, exist_ok=True)
    for name in ["pilot.csv", "trials.csv", "matched.csv", "timing_trials.csv"]:
        (REPORT / name).write_bytes((RESULTS / name).read_bytes())
    with (RESULTS / "matched.csv").open(newline="") as stream:
        matches = list(csv.DictReader(stream))
    with (RESULTS / "trials.csv").open(newline="") as stream:
        trials = list(csv.DictReader(stream))
    panels = [("Reference / 32M photons", None)] + [(r["method"], r) for r in matches]
    edge, header = 256, 84
    canvas = Image.new("RGB", (edge * len(panels), edge + header), "#202020"); draw = ImageDraw.Draw(canvas)
    for i, (label, metrics) in enumerate(panels):
        values = ref[2]
        if metrics:
            row = next(r for r in trials if r["method"] == label and r["phase"] == "match" and r["seed"] == "42")
            _, _, values = common.read_pfm(Path(row["directory"]) / "quality_indirect.pfm")
            draw.text((i * edge + 8, 26), f"3-seed L2: {float(metrics['rms_relative_l2']):.2%}", fill="white")
            draw.text((i * edge + 8, 44), f"9-run median: {float(metrics['median_render_seconds']):.3f}s", fill="white")
            draw.text((i * edge + 8, 62), f"N={metrics['photons']} / indirect only", fill="#b0b0b0")
        draw.text((i * edge + 8, 8), label, fill="white")
        display = Image.new("RGB", (ref[0], ref[1])); pixels = display.load()
        for y in range(ref[1]):
            for x in range(ref[0]):
                offset = ((ref[1] - 1 - y) * ref[0] + x) * 3
                pixels[x, y] = tuple(round(255 * (v / (1 + v)) ** (1 / 2.2)) for v in values[offset:offset + 3])
        canvas.paste(display.resize((edge, edge), Image.Resampling.NEAREST), (i * edge, header))
    canvas.save(REPORT / "comparison.png")
    print(f"Report: {REPORT}")


def main():
    global RESULTS
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=["pilot", "match", "timing", "report", "all"], default="all")
    parser.add_argument("--results", type=Path, default=RESULTS)
    args = parser.parse_args(); RESULTS = args.results.resolve(); RESULTS.mkdir(parents=True, exist_ok=True)
    if not (common.RESULTS / "reference_a/quality_indirect.pfm").exists() or not (common.RESULTS / "reference_b/quality_indirect.pfm").exists():
        common.reference()
    for phase, function in [("pilot", pilot), ("match", match), ("timing", timing), ("report", report)]:
        if args.phase in [phase, "all"]:
            function()


if __name__ == "__main__":
    main()
