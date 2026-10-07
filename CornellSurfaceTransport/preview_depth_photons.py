from pathlib import Path
import csv, math, statistics, struct
from PIL import Image, ImageDraw

root = Path('scratch/depth-photon')
out = Path('reaserch/pbvr_photon_transport/results/2026-10-07/depth_transport')
out.mkdir(parents=True, exist_ok=True)
rows = []
max_flux_error = 0
for seed in (42, 43, 44):
    for drop in (0, 1):
        folder = root / f'check-seed{seed}-drop{drop}'
        row = next(csv.DictReader((folder / 'depth_metrics.csv').open()))
        rows.append(row)
        orders = list(csv.DictReader((folder / 'depth_orders.csv').open()))
        (out / f'orders_seed{seed}_drop{drop}.csv').write_bytes((folder / 'depth_orders.csv').read_bytes())
        for order in orders:
            for c in 'rgb':
                launched = float(order[f'launched_{c}'])
                delta = abs(launched - float(order[f'arrived_{c}']) - float(order[f'escaped_{c}']))
                max_flux_error = max(max_flux_error, delta / max(launched, 1e-30))
with (out / 'metrics.csv').open('w', newline='') as stream:
    writer = csv.DictWriter(stream, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
def pfm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip() == b'PF'
        w, h = map(int, stream.readline().split()); scale = float(stream.readline())
        return struct.unpack(('<' if scale < 0 else '>') + str(w*h*3) + 'f', stream.read())
errors = []
for seed in (42,43,44):
    a = pfm(root / f'check-seed{seed}-drop0/depth_indirect.pfm')
    b = pfm(root / f'check-seed{seed}-drop1/depth_indirect.pfm')
    errors.append(math.sqrt(sum((x-y)**2 for x,y in zip(a,b))/sum(x*x for x in a)))
edge, header = 384, 60
for suffix in ('', '_indirect'):
    canvas = Image.new('RGB', (edge*2, edge+header), '#202020'); draw = ImageDraw.Draw(canvas)
    for drop, label in ((0,'Depth maps: 32 / 32 / 32 / 32'),(1,'Depth maps: 32 / 16 / 8 / 8')):
        row = next(r for r in rows if r['seed']=='42' and r['drop_every']==str(drop))
        draw.text((edge*drop+8, 8), label, fill='white')
        draw.text((edge*drop+8, 27), f"Transport {float(row['transport_seconds']):.3f}s / seed 42", fill='white')
        image = Image.open(root / f'check-seed42-drop{drop}' / f'depth{suffix}.ppm')
        canvas.paste(image.resize((edge,edge), Image.Resampling.NEAREST),(edge*drop,header))
    canvas.save(out / ('comparison'+suffix+'.png'))
for drop in (0,1):
    group = [r for r in rows if r['drop_every']==str(drop)]
    print('drop',drop,'transport',statistics.median(float(r['transport_seconds']) for r in group),
          'total',statistics.median(float(r['total_seconds']) for r in group),
          'stored', [r['stored_photons'] for r in group])
print('Paired indirect L2',errors,'RMS',math.sqrt(statistics.mean(e*e for e in errors)))
print('Max flux relative residual',max_flux_error)
