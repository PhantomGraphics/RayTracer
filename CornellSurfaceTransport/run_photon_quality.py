"""Matched-quality photon transport benchmark (standard library; Pillow for previews).

Run after building CornellSurfaceTransport. Phases cache raw renders in scratch.
"""
import argparse
import array
import csv
import math
import random
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
EXE = ROOT / "Phantom/build/windows-release/RayTracer/CornellSurfaceTransport.exe"
RESULTS = ROOT / "scratch/photon-matched-quality"
SEEDS = [42, 43, 44]
METHODS = {
    "path": ("path", 1.0, 0.0),
    "pbvr_capped": ("pbvr", 0.65, 0.2),
    "pbvr_65": ("pbvr", 0.65, 0.0),
    "pbvr_80": ("pbvr", 0.8, 0.0),
    "pbvr_50": ("pbvr", 0.5, 0.0),
}


def read_pfm(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Not RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        values = array.array("f")
        values.frombytes(stream.read())
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    if len(values) != width * height * 3 or not all(math.isfinite(v) and v >= 0 for v in values):
        raise ValueError(f"Invalid image: {path}")
    return width, height, values


def render(label, mode, count, seed, retention=1.0, cap=0, ensembles=1):
    destination = RESULTS / label
    metric = destination / "quality_metrics.csv"
    if not metric.exists():
        command = [str(EXE), "--photon-quality", str(destination), "96", str(count), "0.12", "32",
                   str(seed), mode, str(retention), str(cap), str(ensembles), "uniform", "0", "1", "1"]
        print(f"Running {label}", flush=True)
        subprocess.run(command, check=True)
    with metric.open(newline="", encoding="utf-8-sig") as stream:
        row = next(csv.DictReader(stream))
    if (row["mode"] != mode or int(row["photon_count"]) != count or int(row["seed"]) != seed
            or int(row["ensembles"]) != ensembles or int(row["cap"]) != cap
            or float(row["retention"]) != retention or int(row["size"]) != 96
            or float(row["radius"]) != 0.12):
        raise ValueError(f"Cached parameters differ: {destination}. Use a fresh --results directory.")
    row["directory"] = str(destination)
    return row


def reference():
    for label, seed in [("reference_a", 1001), ("reference_b", 9001)]:
        render(label, "path", 500000, seed, ensembles=32)


def reference_image():
    wa, ha, a = read_pfm(RESULTS / "reference_a/quality_indirect.pfm")
    wb, hb, b = read_pfm(RESULTS / "reference_b/quality_indirect.pfm")
    if (wa, ha) != (wb, hb):
        raise ValueError("Reference dimensions differ")
    mean = [(x + y) * 0.5 for x, y in zip(a, b)]
    denominator = sum(v * v for v in mean)
    delta = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / denominator)
    print(f"Independent 16M-photon reference difference: {delta:.6%}; combined reference: 32M photons", flush=True)
    return wa, ha, mean, denominator, delta


def evaluate(row, ref):
    w, h, values = read_pfm(Path(row["directory"]) / "quality_indirect.pfm")
    rw, rh, target, denominator, _ = ref
    if (w, h) != (rw, rh):
        raise ValueError("Candidate dimensions differ")
    result = dict(row)
    result["relative_l2"] = math.sqrt(sum((a - b) ** 2 for a, b in zip(values, target)) / denominator)
    result["mean_ratio"] = sum(values) / sum(target)
    return result


def run_group(method, count, ref, ensembles=1, prefix="pilot", seeds=SEEDS):
    mode, retention, fraction = METHODS[method]
    rows = []
    for seed in seeds:
        label = f"{prefix}-{method}-n{count}-e{ensembles}-seed{seed}"
        row = render(label, mode, count, seed, retention, round(count * fraction), ensembles)
        row = evaluate(row, ref)
        row["method"] = method
        row["phase"] = prefix
        rows.append(row)
    rms = math.sqrt(statistics.mean(float(row["relative_l2"]) ** 2 for row in rows))
    duration = statistics.median(float(row["render_total_seconds"]) for row in rows)
    print(f"{method} N={count} E={ensembles}: RMS relative L2={rms:.5%}, median total={duration:.6f}s", flush=True)
    return rows, rms, duration


def write_csv(path, rows):
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def pilot():
    ref = reference_image()
    rows = []
    for method in METHODS:
        group, _, _ = run_group(method, 50000, ref)
        rows.extend(group)
    write_csv(RESULTS / "pilot.csv", rows)


def match():
    ref = reference_image()
    with (RESULTS / "pilot.csv").open(newline="") as stream:
        pilots = list(csv.DictReader(stream))
    output, matches = [], []
    # Evaluate the original capped configuration and the best uncapped PBVR
    # configuration. Selection uses pilot variance * measured indirect time.
    scores = {}
    for method in METHODS:
        group = [row for row in pilots if row["method"] == method]
        mse = statistics.mean(float(row["relative_l2"]) ** 2 for row in group)
        time = statistics.median(float(row["indirect_total_seconds"]) for row in group)
        scores[method] = mse * time
    best = min((m for m in METHODS if m.startswith("pbvr_") and m != "pbvr_capped"), key=scores.get)
    print(f"Best uncapped pilot efficiency: {best}; scores={scores}", flush=True)
    METHODS["pbvr_capped_ensemble"] = METHODS["pbvr_capped"]
    for goal in [0.10, 0.05]:
        for method in ["path", "pbvr_capped", best, "pbvr_capped_ensemble"]:
            pilot_method = "pbvr_capped" if method == "pbvr_capped_ensemble" else method
            group = [row for row in pilots if row["method"] == pilot_method]
            pilot_mse = statistics.mean(float(row["relative_l2"]) ** 2 for row in group)
            ensemble_mode = method == "pbvr_capped_ensemble"
            count = 50000 if ensemble_mode else max(10000, round(50000 * pilot_mse / (goal * goal) / 1000) * 1000)
            ensembles = max(1, round(pilot_mse / (goal * goal))) if ensemble_mode else 1
            # Refine using separate tuning seeds; reported trials use 42..44.
            for attempt in range(3):
                tuning, rms, _ = run_group(method, count, ref, ensembles, prefix=f"tune{round(goal*100)}", seeds=[142, 143, 144])
                output.extend(tuning)
                if abs(rms / goal - 1) <= 0.035:
                    break
                if ensemble_mode:
                    revised = max(1, round(ensembles * (rms / goal) ** 2))
                    if revised == ensembles:
                        break
                    ensembles = revised
                else:
                    revised = max(10000, round(count * (rms / goal) ** 2 / 1000) * 1000)
                    if revised == count:
                        break
                    count = revised
            trials, rms, median = run_group(method, count, ref, ensembles, prefix=f"match{round(goal*100)}")
            output.extend(trials)
            matches.append({
                "target_l2": goal, "method": method, "photons_per_ensemble": count, "ensembles": ensembles,
                "rms_relative_l2": rms, "median_indirect_seconds": statistics.median(float(r["indirect_total_seconds"]) for r in trials),
                "median_render_seconds": median, "min_render_seconds": min(float(r["render_total_seconds"]) for r in trials),
                "max_render_seconds": max(float(r["render_total_seconds"]) for r in trials),
                "mean_ratio": statistics.mean(float(r["mean_ratio"]) for r in trials),
            })
    write_csv(RESULTS / "trials.csv", output)
    write_csv(RESULTS / "matched.csv", matches)


def report():
    from PIL import Image, ImageDraw
    ref = reference_image()
    destination = ROOT / "reaserch/pbvr_photon_transport/results/2026-10-07/matched_quality"
    destination.mkdir(parents=True, exist_ok=True)
    for name in ["pilot.csv", "trials.csv", "matched.csv", "timing_trials.csv"]:
        if not (RESULTS / name).exists():
            continue
        (destination / name).write_bytes((RESULTS / name).read_bytes())
    reference_rows = []
    for label in ["reference_a", "reference_b"]:
        with (RESULTS / label / "quality_metrics.csv").open(newline="") as stream:
            row = next(csv.DictReader(stream))
        row["reference_label"] = label
        row["reference_pair_relative_l2"] = ref[-1]
        reference_rows.append(row)
    write_csv(destination / "reference.csv", reference_rows)
    with (RESULTS / "matched.csv").open(newline="") as stream:
        matches = list(csv.DictReader(stream))
    with (RESULTS / "trials.csv").open(newline="") as stream:
        trials = list(csv.DictReader(stream))
    names = {"path": "Conventional roulette", "pbvr_capped": "PBVR / p=0.65 + cap",
             "pbvr_80": "PBVR / p=0.80 / no cap", "pbvr_65": "PBVR / p=0.65 / no cap",
             "pbvr_50": "PBVR / p=0.50 / no cap", "pbvr_capped_ensemble": "PBVR / capped ensembles"}

    def display(values, width, height, edge):
        result = Image.new("RGB", (width, height))
        pixels = result.load()
        for y in range(height):
            for x in range(width):
                offset = ((height - 1 - y) * width + x) * 3
                pixels[x, y] = tuple(round(255 * (v / (1 + v)) ** (1 / 2.2)) for v in values[offset:offset + 3])
        return result.resize((edge, edge), Image.Resampling.NEAREST)

    for goal in [0.1, 0.05]:
        group = [r for r in matches if float(r["target_l2"]) == goal]
        edge, header = 288, 80
        canvas = Image.new("RGB", (edge * (len(group) + 1), edge + header), "#202020")
        draw = ImageDraw.Draw(canvas)
        draw.text((8, 8), "Independent 32M-photon reference", fill="white")
        draw.text((8, 27), "Indirect only / fixed radius=0.12", fill="#b0b0b0")
        canvas.paste(display(ref[2], ref[0], ref[1], edge), (0, header))
        for i, metrics in enumerate(group, 1):
            method = metrics["method"]
            selected = next(r for r in trials if r["phase"] == f"match{round(goal*100)}" and r["method"] == method and r["seed"] == "42")
            width, height, values = read_pfm(Path(selected["directory"]) / "quality_indirect.pfm")
            draw.text((i * edge + 8, 8), names[method], fill="white")
            draw.text((i * edge + 8, 27), f"3-seed RMS L2: {float(metrics['rms_relative_l2']):.2%}", fill="white")
            draw.text((i * edge + 8, 44), f"Median render: {float(metrics['median_render_seconds']):.3f}s", fill="white")
            draw.text((i * edge + 8, 61), f"N={metrics['photons_per_ensemble']} x E={metrics['ensembles']}", fill="#b0b0b0")
            canvas.paste(display(values, width, height, edge), (i * edge, header))
        canvas.save(destination / f"comparison_{round(goal*100)}percent.png")
    print(f"Report artifacts: {destination}")


def repeat_timing():
    # Small time differences need an interleaved check for run-order effects.
    ref = reference_image()
    with (RESULTS / "matched.csv").open(newline="") as stream:
        matches = list(csv.DictReader(stream))
    with (RESULTS / "trials.csv").open(newline="") as stream:
        trials = list(csv.DictReader(stream))
    METHODS["pbvr_capped_ensemble"] = METHODS["pbvr_capped"]
    jobs = [(repeat, row, seed) for repeat in [1, 2] for row in matches
            if float(row["target_l2"]) == 0.05 for seed in SEEDS]
    random.Random(314159).shuffle(jobs)
    repeats = []
    for repeat, row, seed in jobs:
        group, _, _ = run_group(row["method"], int(row["photons_per_ensemble"]), ref,
                               int(row["ensembles"]), prefix=f"timing{repeat}", seeds=[seed])
        repeats.extend(group)
    write_csv(RESULTS / "timing_trials.csv", repeats)
    for row in matches:
        if float(row["target_l2"]) != 0.05:
            continue
        group = [r for r in trials if r["phase"] == "match5" and r["method"] == row["method"]]
        group += [r for r in repeats if r["method"] == row["method"]]
        row["median_indirect_seconds"] = statistics.median(float(r["indirect_total_seconds"]) for r in group)
        row["median_render_seconds"] = statistics.median(float(r["render_total_seconds"]) for r in group)
        row["min_render_seconds"] = min(float(r["render_total_seconds"]) for r in group)
        row["max_render_seconds"] = max(float(r["render_total_seconds"]) for r in group)
    write_csv(RESULTS / "matched.csv", matches)


def main():
    global RESULTS
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=["reference", "pilot", "match", "timing", "report", "all"], default="all")
    parser.add_argument("--results", type=Path, default=RESULTS, help="Use a new directory to rerun timings instead of cached results.")
    args = parser.parse_args()
    RESULTS = args.results.resolve()
    RESULTS.mkdir(parents=True, exist_ok=True)
    if args.phase in ("reference", "all"):
        reference()
    if args.phase in ("pilot", "all"):
        pilot()
    if args.phase in ("match", "all"):
        match()
    if args.phase in ("timing", "all"):
        repeat_timing()
    if args.phase in ("report", "all"):
        report()


if __name__ == "__main__":
    main()
