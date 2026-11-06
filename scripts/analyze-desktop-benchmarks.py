#!/usr/bin/env python3
"""Compare desktop logs without turning missing/failed/invalid metrics into wins."""
import argparse
import csv
from pathlib import Path

def read(path):
    text=path.read_text(errors='replace')
    latency={};throughput={};notices=[];context=[]
    for line in text.splitlines():
        if line.startswith(('LATENCY ','THROUGHPUT ')):
            fields=dict(p.split('=',1) for p in line.split()[1:] if '=' in p)
            (latency if line.startswith('LATENCY ') else throughput)[fields['name']]=fields
        elif line.startswith(('RESULT ','SUITE ')):notices.append(line)
        elif line.startswith(('ENV ','GRAPHICS protocol=','GRAPHICS renderer=','FONT file=','APP phase=')):context.append(line)
    invalid={}
    gpu=latency.get('gl_scene_and_blend_GPU_timer')
    if gpu:
        wall=sum(float(latency.get(name,{}).get('max',0)) for name in
                 ('gl_scene_and_12layer_blend_submit','gl_finish_completion_wait'))
        if float(gpu['max'])>max(1000000,wall*10):
            invalid['gl_scene_and_blend_GPU_timer']='impossible_device_timer_outlier_exceeds_completion_wall_time'
    oml=latency.get('gl_OML_swap_completion_interval');wall=latency.get('gl_frame_completion_interval_wall')
    if oml and wall and float(oml['mean'])>10*float(wall['mean']):
        invalid['gl_OML_swap_completion_interval']='OML_timeline_inconsistent_with_wall_intervals_not_a_physical_presentation_score'
    return latency,throughput,notices,context,invalid

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('cachyos',type=Path);p.add_argument('avoryos',type=Path);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();linux=read(args.cachyos);avory=read(args.avoryos)
    def graphics_protocol(data):
        return next((line for line in data[3] if line.startswith('GRAPHICS protocol=')),
                    'legacy_queries_in_completion')
    if graphics_protocol(linux) != graphics_protocol(avory):
        for data in (linux,avory):
            for name in ('gl_finish_completion_wait','gl_scene_and_12layer_blend_submit'):
                if name in data[0]:
                    data[4][name]='graphics_protocol_mismatch_query_instrumentation'

    lines=['# Desktop benchmark comparison','',
           '1000 samples per latency workload, two CPU/memory load workers; browser/Wine use three runs.',
           'CachyOS: native, 12 logical CPUs, Btrfs, NVIDIA RTX 4060 Ti, active host compositor.',
           'AvoryOS: KVM, four vCPUs, 4 GiB, ext4 on virtual NVMe, virgl on the same RTX 4060 Ti, XFWM compositor.',
           'Storage paths, graphics/presentation stacks, and default fonts differ. These are environment measurements, not an overall OS score.',
           'Chromium was staged privately from the host so the actual browser build matches. AvoryOS root uses --no-sandbox only in the disposable guest.',
           '', '## Latency (microseconds; lower is better)','',
           '| Workload | CachyOS p50 | AvoryOS p50 | CachyOS p99 | AvoryOS p99 |',
           '| --- | ---: | ---: | ---: | ---: |']
    names=list(dict.fromkeys([*linux[0],*avory[0]]))
    def value(data,name,key):
        if name in data[4]:return 'INVALID'
        row=data[0].get(name);return row.get(key,'—') if row else '—'
    for name in names:
        lines.append('| '+name+' | '+' | '.join(value(data,name,key) for key,data in
                    [('p50',linux),('p50',avory),('p99',linux),('p99',avory)])+' |')
    lines+=['','Count=1 initialization measurements have no statistical tail meaning. Browser/Wine count=3 tails are not stable percentile estimates.',
            'The frame/action-to-swap wall values are proxies. Different refresh/presentation paths prevent direct responsiveness ranking from those rows.',
            '', '## Throughput (MiB/s)','', '| Workload | CachyOS | AvoryOS |','| --- | ---: | ---: |']
    for name in dict.fromkeys([*linux[1],*avory[1]]):
        lines.append(f'| {name} | {linux[1].get(name,{}).get("MiB_s","—")} | {avory[1].get(name,{}).get("MiB_s","—")} |')
    for label,data in [('CachyOS',linux),('AvoryOS',avory)]:
        lines+=['',f'## {label} coverage and validity','', '```text',*data[2],
                *[f'INVALID {name}: {reason}' for name,reason in data[4].items()],
                '```','', '```text',*data[3],'```']
    args.output.write_text('\n'.join(lines)+'\n')
    with args.output.with_suffix('.csv').open('w',newline='') as f:
        w=csv.writer(f);w.writerow(['workload','cachyos_count','avoryos_count','cachyos_p50_us','avoryos_p50_us','cachyos_p99_us','avoryos_p99_us'])
        for name in names:w.writerow([name,linux[0].get(name,{}).get('count',''),avory[0].get(name,{}).get('count',''),
                                     value(linux,name,'p50'),value(avory,name,'p50'),value(linux,name,'p99'),value(avory,name,'p99')])
    print(args.output)

if __name__=='__main__':main()
