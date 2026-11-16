import collections,csv,hashlib,json,math,pathlib,statistics,sys,time,zipfile

root=pathlib.Path('/home/nvidia/jetson-inference-edgevision')
day=root/'projects/edgevision/artifacts/2026-11-16'
batch=day/'m5-formal-matrix-r1'
out=day/'m5-formal-review-r1'
out.mkdir(exist_ok=False)
def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()
def quantile(values,p):return values[max(0,math.ceil(len(values)*p)-1)]
complete=json.loads((batch/'complete.json').read_text())
assert complete=={'normal_exit':True,'runs':15},complete
manifest=json.loads((batch/'manifest.json').read_text())
assert not manifest['simulated'] and not manifest['profiler'] and manifest['formal_duration_requested']
assert manifest['argv']['seconds']==300 and manifest['argv']['repeats']==3
for field,arg in [('binary_sha256','binary'),('engine_sha256','engine'),('model_sha256','model')]:
    assert sha(root/manifest['argv'][arg])==manifest[field],field
assert sha(root/manifest['argv']['corpus']/'inputs.tsv')==manifest['corpus_list_sha256']
runs=json.loads((batch/'runs.json').read_text());assert len(runs)==15 and all(r['exit_code']==0 for r in runs)
rows=[];grouped=collections.defaultdict(list);total=0
for record in runs:
    label=record['label'];folder=batch/label;summary=json.loads((folder/'summary.json').read_text())
    assert summary['normal_exit'] and summary['measured_ns']>=300e9 and summary['requested_measured_seconds']==300
    assert summary['warmup_excluded']==60 and summary['bounded_timing_csv_included_in_throughput']
    values=[];ordinals=set();buckets=collections.defaultdict(list);base=None
    with (folder/'timings.csv').open() as stream:
        for row in csv.DictReader(stream):
            ordinal=int(row['ordinal']);begin=int(row['started_mono_ns']);end=int(row['finished_mono_ns']);latency=int(row['latency_ns'])
            assert ordinal not in ordinals and end>=begin and latency==end-begin and latency>0
            assert all(math.isfinite(float(row[k])) and float(row[k])>=0 for k in ('copy_ms','preprocess_ms','inference_ms','critical_path_gpu_ms'))
            ordinals.add(ordinal);values.append(latency)
            if base is None:base=begin
            buckets[int((begin-base)//60_000_000_000)].append(latency)
    assert len(values)==summary['completed'] and ordinals==set(range(summary['completed']))
    values.sort()
    for percent in (50,95,99):assert quantile(values,percent/100)==summary[f'latency_p{percent}_ns']
    calculated=len(values)*1e9/summary['measured_ns']
    assert abs(calculated-summary['throughput_per_s'])<1e-6
    windows=[]
    for index,latencies in sorted(buckets.items()):
        latencies.sort();windows.append({'minute_bin_by_host_start':index,'samples':len(latencies),
            'p50_ms':quantile(latencies,.5)/1e6,'p95_ms':quantile(latencies,.95)/1e6,'p99_ms':quantile(latencies,.99)/1e6,
            'max_ms':latencies[-1]/1e6,'note':'drain tail / partial bin' if index>=5 else '60s start-time bucket'})
    result={'run':label,**summary,'csv_rows_verified':len(values),'latency_max_ms':values[-1]/1e6,'minute_latency_windows':windows}
    rows.append(result);grouped[f"{summary['mode']}-{summary['memory']}"] .append(result);total+=len(values)
groups={}
for name,records in grouped.items():
    assert len(records)==3
    groups[name]={'runs':[r['run'] for r in records],'samples':sum(r['completed'] for r in records),
        'throughput_per_s_each':[r['throughput_per_s'] for r in records],
        'throughput_per_s_median':statistics.median(r['throughput_per_s'] for r in records),
        'p95_ms_each':[r['latency_p95_ns']/1e6 for r in records],
        'p99_ms_each':[r['latency_p99_ns']/1e6 for r in records],
        'p99_ms_median':statistics.median(r['latency_p99_ns']/1e6 for r in records)}
numeric=collections.defaultdict(list);unavailable=collections.Counter();mem_available=[];times=[];walls=[]
for line in (batch/'conditions.jsonl').open():
    point=json.loads(line);times.append(point['mono_ns']);walls.append(point['wall_utc'])
    for path,value in point['values'].items():
        if isinstance(value,dict):unavailable[path]+=1
        elif value.lstrip('-').isdigit():numeric[path].append(int(value))
        elif path=='/proc/meminfo':
            field=next((v for v in value.splitlines() if v.startswith('MemAvailable:')),None)
            if field:mem_available.append(int(field.split()[1]))
telemetry={'samples':len(times),'first_wall_utc':walls[0],'last_wall_utc':walls[-1],
    'sample_intervals_s':{'min':min(b-a for a,b in zip(times,times[1:]))/1e9,
        'median':statistics.median(b-a for a,b in zip(times,times[1:]))/1e9,'max':max(b-a for a,b in zip(times,times[1:]))/1e9},
    'numeric_ranges':{p:{'min':min(v),'max':max(v),'samples':len(v)} for p,v in numeric.items()},
    'unavailable_samples':dict(unavailable),
    'system_mem_available_kib':{'min':min(mem_available),'max':max(mem_available)},
    'missing':['per-process RSS and CPU utilization','GPU utilization','power rails'],
    'missing_reason':'This frozen sampler did not collect these metrics; loadavg is not CPU utilization and MemAvailable is not process RSS. Resource comparison remains a separate required gate.'}
review={'schema_version':1,'stage':'M5','scope':'offline fixed RGB corpus, same common display NMS; runtime camera remains P1',
    'formal_duration_and_repetition_gate_passed':True,'all_resource_gates_complete':False,
    'runs_verified':15,'total_measured_completions':total,'source_zip_sha256':sha(day/'m5-formal-source-r3.zip'),
    'run_manifest':manifest,'groups':groups,'runs':rows,'telemetry':telemetry,
    'limitations':['Offline images/s is not USB camera FPS.','CSV writes excluded from per-frame latency but included in throughput. Image/tensor dumping disabled.',
        'P0 event zeros mean not instrumented, not zero GPU execution time.','Group median P99 is descriptive; individual and minute-window distributions are retained.',
        'No formal ROS2 or Agent-interference/endurance result is implied.','Resource comparison still requires CPU/GPU utilization, per-process RSS and readable power sampling.']}
(out/'review.json').write_text(json.dumps(review,indent=2)+'\n')
inputs=[(p,p.relative_to(day)) for p in batch.rglob('*') if p.is_file()]
inputs+=[(day/'m5-formal-source-r3.zip',pathlib.Path('m5-formal-source-r3.zip')),(out/'review.json',pathlib.Path('m5-formal-review-r1/review.json'))]
file_manifest={str(name):{'bytes':path.stat().st_size,'sha256':sha(path)} for path,name in inputs}
(out/'raw-file-manifest.json').write_text(json.dumps(file_manifest,indent=2)+'\n')
archive=day/'m5-formal-raw-evidence-r1.zip'
with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as zipped:
    for path,name in inputs:zipped.write(path,name)
    zipped.write(out/'raw-file-manifest.json','m5-formal-review-r1/raw-file-manifest.json')
print(json.dumps({'normal_exit':True,'runs':15,'completions':total,'groups':groups,
    'review_path':str(out),'raw_zip':str(archive),'bytes':archive.stat().st_size,'sha256':sha(archive)},indent=2))
