"""Small paired projection experiment, not a matched-quality benchmark.

Run from the repository root after building CornellSurfaceTransport and shaders.
Pillow is used only for the report image. Raw renders remain under scratch/.
"""
import argparse
import csv
import math
import random
import statistics
import struct
import subprocess
from pathlib import Path

METHODS = {
    'hemicube32': ('hemicube', 32, 8, 3),
    'paraboloid32_s2': ('paraboloid', 32, 8, 2),
    'paraboloid32_s3': ('paraboloid', 32, 8, 3),
    'paraboloid32_s4': ('paraboloid', 32, 8, 4),
    'paraboloid64_s3': ('paraboloid', 64, 16, 3),
}
EXE = Path('Phantom/build/windows-release/RayTracer/CornellSurfaceTransport.exe').resolve()
REPORT = Path('reaserch/pbvr_photon_transport/results/2026-10-07/paraboloid_transport')

def write_csv(path, rows):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader(); writer.writerows(rows)

def pfm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip() == b'PF'
        w, h = map(int, stream.readline().split()); scale = float(stream.readline())
        return struct.unpack(('<' if scale < 0 else '>') + str(w*h*3) + 'f', stream.read())

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--results', type=Path, default=Path('scratch/paraboloid-photon/comparison'))
    parser.add_argument('--phase', choices=('run', 'report', 'all'), default='all')
    args = parser.parse_args(); args.results.mkdir(parents=True, exist_ok=True)
    if args.phase in ('run', 'all'):
        jobs = [(name, seed, repeat) for name in METHODS for seed in (42,43,44) for repeat in range(3)]
        random.Random(271828).shuffle(jobs)
        rows = []
        for name, seed, repeat in jobs:
            projection, edge, minimum, subdivision = METHODS[name]
            directory = args.results / f'{name}-seed{seed}-r{repeat}'
            command = [str(EXE), '--photon-depth', str(directory), '96', '8', str(edge),
                       str(minimum), '4', '32', str(seed), '0.12', '1', projection, str(subdivision)]
            completed = subprocess.run(command, capture_output=True, text=True)
            if completed.returncode:
                raise RuntimeError(completed.stdout + completed.stderr)
            row = next(csv.DictReader((directory / 'depth_metrics.csv').open()))
            orders = list(csv.DictReader((directory / 'depth_orders.csv').open()))
            row.update(method=name, repeat=repeat, directory=str(directory.resolve()))
            row['depth_maps'] = sum(int(s['depth_maps']) for s in orders)
            row['max_relative_flux_residual'] = max(
                abs(float(s[f'launched_{c}']) - float(s[f'arrived_{c}']) - float(s[f'escaped_{c}']))
                / max(float(s[f'launched_{c}']),1e-30) for s in orders for c in 'rgb')
            row['first_arrived_ratio'] = float(orders[0]['arrived_r']) / float(orders[0]['launched_r'])
            rows.append(row)
            print(name, seed, repeat, row['transport_seconds'], flush=True)
        write_csv(args.results / 'trials.csv', rows)
    if args.phase not in ('report', 'all'):
        return
    from PIL import Image, ImageDraw
    rows = list(csv.DictReader((args.results / 'trials.csv').open()))
    summaries = []
    for name in METHODS:
        group = [r for r in rows if r['method']==name]
        differences = []
        for seed in (42,43,44):
            a = next(r for r in rows if r['method']=='hemicube32' and r['seed']==str(seed) and r['repeat']=='0')
            b = next(r for r in group if r['seed']==str(seed) and r['repeat']=='0')
            av,bv = pfm(Path(a['directory'])/'depth_indirect.pfm'),pfm(Path(b['directory'])/'depth_indirect.pfm')
            differences.append(math.sqrt(sum((x-y)**2 for x,y in zip(av,bv))/sum(x*x for x in av)))
        summary = {'method':name, 'rms_paired_indirect_difference':math.sqrt(statistics.mean(x*x for x in differences))}
        for field in ('transport_seconds','total_seconds','stored_photons','depth_maps','first_arrived_ratio'):
            summary['median_'+field] = statistics.median(float(r[field]) for r in group)
        summary['max_relative_flux_residual'] = max(float(r['max_relative_flux_residual']) for r in group)
        summaries.append(summary)
    REPORT.mkdir(parents=True, exist_ok=True)
    write_csv(REPORT/'trials.csv', rows); write_csv(REPORT/'summary.csv', summaries)
    for suffix in ('','_indirect'):
        edge,header = 320,72
        canvas=Image.new('RGB',(edge*3,(edge+header)*2),'#202020'); draw=ImageDraw.Draw(canvas)
        for i,summary in enumerate(summaries):
            name=summary['method']; x=(i%3)*edge; y=(i//3)*(edge+header)
            row=next(r for r in rows if r['method']==name and r['seed']=='42' and r['repeat']=='0')
            draw.text((x+8,y+8),name,fill='white')
            draw.text((x+8,y+26),f"9-run transport median: {summary['median_transport_seconds']:.3f}s",fill='white')
            draw.text((x+8,y+44),f"Paired indirect difference: {summary['rms_paired_indirect_difference']:.1%}",fill='white')
            image=Image.open(Path(row['directory'])/f'depth{suffix}.ppm')
            canvas.paste(image.resize((edge,edge),Image.Resampling.NEAREST),(x,y+header))
        canvas.save(REPORT/f'comparison{suffix}.png')
    for row in summaries:
        print(row)

if __name__ == '__main__':
    main()
