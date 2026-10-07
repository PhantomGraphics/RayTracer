"""Report an actual RayTracerView progressive demo; never invokes a renderer."""
import argparse
import csv
import math
import statistics
import struct
from pathlib import Path

def pfm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip()==b'PF'
        w,h=map(int,stream.readline().split()); scale=float(stream.readline())
        return struct.unpack(('<' if scale<0 else '>')+str(w*h*3)+'f',stream.read())

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('directory',type=Path)
    parser.add_argument('--output',type=Path,default=Path('reaserch/pbvr_photon_transport/results/2026-10-07/progressive_view'))
    args=parser.parse_args(); folder=args.directory; out=args.output; out.mkdir(parents=True,exist_ok=True)
    reference=pfm(folder/'reference_indirect.pfm'); denominator=sum(v*v for v in reference)
    rows=list(csv.DictReader((folder/'passes.csv').open()))
    print('Viewer timings:',(folder/'ui_timings.csv').read_text())
    summary=[]
    for count in (1,4,16,32):
        values=pfm(folder/f'refine_{count}_indirect.pfm')
        assert len(values)==len(reference) and all(math.isfinite(x) for x in values)
        error=math.sqrt(sum((a-b)**2 for a,b in zip(values,reference))/max(denominator,1e-30))
        metrics=next(r for r in rows if r['quality']=='refine' and int(r['pass'])==count)
        summary.append({'passes':count,'relative_indirect_l2':error,'estimated_relative_standard_error':metrics['estimated_relative_standard_error']})
    with (out/'quality.csv').open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=summary[0].keys()); writer.writeheader(); writer.writerows(summary)
    frames=[float(r['seconds']) for r in csv.DictReader((folder/'frames.csv').open()) if float(r['seconds'])>0]
    ordered=sorted(frames)
    def percentile(p): return ordered[min(len(ordered)-1,math.ceil(p*len(ordered))-1)]
    stats={'median_frame_ms':statistics.median(frames)*1000,'p95_frame_ms':percentile(.95)*1000,
           'max_frame_ms':max(frames)*1000,'median_refine_pass_ms':statistics.median(float(r['seconds']) for r in rows if r['quality']=='refine')*1000}
    with (out/'performance.csv').open('w',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=stats.keys()); writer.writeheader(); writer.writerow(stats)
    for name in ('passes.csv','frames.csv','ui_timings.csv','view_32.png','view_after_move.png'):
        if (folder/name).exists(): (out/name).write_bytes((folder/name).read_bytes())
    from PIL import Image,ImageDraw
    names=['preview','refine_1','refine_4','refine_16','refine_32','reference']; edge,header=256,60
    canvas=Image.new('RGB',(edge*3,(edge+header)*2),'#202020'); draw=ImageDraw.Draw(canvas)
    for i,name in enumerate(names):
        x=(i%3)*edge; y=(i//3)*(edge+header)
        image=Image.open(folder/f'{name}.png'); canvas.paste(image.resize((edge,edge),Image.Resampling.NEAREST),(x,y+header))
        draw.text((x+8,y+8),name,fill='white')
        if name.startswith('refine_'):
            row=next(r for r in summary if r['passes']==int(name.split('_')[1]))
            draw.text((x+8,y+26),f"Indirect difference: {row['relative_indirect_l2']:.1%}",fill='white')
        elif name=='reference': draw.text((x+8,y+26),'32 independent passes / same approximation',fill='white')
    canvas.save(out/'comparison.png')
    print('Quality:',summary); print('Performance:',stats)

if __name__=='__main__': main()
