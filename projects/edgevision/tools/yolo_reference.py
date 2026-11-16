#!/usr/bin/env python3
"""Frozen-input YOLO M3 correctness reference. No performance measurements.

Install tools/requirements-model-reference.txt in an isolated environment first.
prepare: audit the selected image archive and freeze RGB/letterbox/tensor bytes.
reference: ONNX Runtime CPU, independent NumPy class-aware NMS and COCOeval.
compare: compare target C++ parsing, FP16 outputs and the SAME 200-image AP.
"""
import argparse
import hashlib
import json
import pathlib
import sys
import zipfile

import cv2
import numpy as np


def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha(path):
    return sha_bytes(path.read_bytes())


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def prepare(args):
    if sha(args.package) != args.package_sha256:
        raise ValueError('DATASET_PACKAGE_HASH_MISMATCH')
    args.out.mkdir(parents=True, exist_ok=False)
    corpus = args.out / 'corpus'; corpus.mkdir()
    with zipfile.ZipFile(args.package) as z:
        manifest = json.loads(z.read('dataset-manifest.json'))
        annotations = json.loads(z.read('instances_subset.json'))
        save(args.out / 'dataset-manifest.json', manifest)
        save(args.out / 'instances_subset.json', annotations)
        samples = []
        for im in manifest['images']:
            raw = z.read('images/' + im['file_name'])
            if sha_bytes(raw) != im['sha256']:
                raise ValueError('IMAGE_HASH_MISMATCH')
            bgr = cv2.imdecode(np.frombuffer(raw, np.uint8), cv2.IMREAD_COLOR)
            if bgr is None or bgr.shape[:2] != (im['height'], im['width']):
                raise ValueError('IMAGE_GEOMETRY_MISMATCH')
            samples.append((im['image_id'], cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB), im['fixed_regression'], 'coco'))
    # Algorithmic test images, separate from the annotated accuracy set.
    for iid, shape, color in [(1000001,(360,640),(0,0,0)), (1000002,(337,181),(114,114,114)),
                              (1000003,(181,337),(255,0,0)), (1000004,(641,479),(0,0,255))]:
        rgb = np.empty((*shape,3),np.uint8); rgb[:] = color
        samples.append((iid,rgb,True,'synthetic_uniform'))
    rows, records = [], []
    for iid, rgb, fixed, kind in samples:
        h,w = rgb.shape[:2]; scale = min(640/w,640/h)
        rw,rh = round(w*scale), round(h*scale)
        left,top = round((640-rw)/2-.1), round((640-rh)/2-.1)
        right,bottom = 640-rw-left,640-rh-top
        resized = cv2.resize(rgb,(rw,rh),interpolation=cv2.INTER_LINEAR)
        padded = cv2.copyMakeBorder(resized,top,bottom,left,right,cv2.BORDER_CONSTANT,value=(114,114,114))
        chw = np.ascontiguousarray(padded.transpose(2,0,1))
        normalized = chw.astype(np.float32) / np.float32(255)
        (corpus / f'{iid}.u8').write_bytes(chw.tobytes())
        if fixed:
            (corpus / f'{iid}.rgb').write_bytes(rgb.tobytes())
        rows.append(f'{iid}\t{w}\t{h}\t{left}\t{top}\t{scale:.12g}\t{int(fixed)}\n')
        records.append({'image_id': iid, 'width':w,'height':h,'left':left,'top':top,'scale':scale,
                        'resized_width':rw,'resized_height':rh,'fixed':fixed,'kind':kind,
                        'rgb_sha256':sha_bytes(rgb.tobytes()),'u8_sha256':sha_bytes(chw.tobytes()),
                        'f32_sha256':sha_bytes(normalized.tobytes())})
    (corpus / 'inputs.tsv').write_text(''.join(rows),encoding='utf-8',newline='\n')
    info = {'source_pack_sha256':sha(args.package),'opencv_version':cv2.__version__,
            'numpy_version':np.__version__,'records':records}
    save(args.out/'input-manifest.json',info)
    package = args.out / 'target-inputs.zip'
    with zipfile.ZipFile(package,'w',zipfile.ZIP_DEFLATED) as z:
        for p in sorted(corpus.iterdir()): z.write(p,'corpus/'+p.name)
        z.write(args.out/'input-manifest.json','input-manifest.json')
    save(args.out/'target-package.json',{'sha256':sha(package),'bytes':package.stat().st_size})
    print(json.dumps({'images':len(records),'fixed':sum(i['fixed'] for i in records),
                      'target_package_bytes':package.stat().st_size,'target_package_sha256':sha(package)}))


def decode(raw, im, confidence=.25, multi_label=False):
    """Independent NumPy parser; coordinates/NMS follow the frozen contract."""
    x = np.asarray(raw, np.float32).reshape(84,8400)
    if not np.isfinite(x).all() or (x[2:4]<0).any() or (x[4:]<0).any() or (x[4:]>1).any():
        raise ValueError('INVALID_MODEL_OUTPUT')
    scores = x[4:]
    if multi_label:
        classes,anchors = np.nonzero(scores > np.float32(confidence))
    else:
        best = scores.argmax(axis=0)
        anchors = np.flatnonzero(scores[best,np.arange(8400)] > np.float32(confidence))
        classes = best[anchors]
    s = scores[classes,anchors]
    xy = x[:2,anchors].T; wh = x[2:4,anchors].T
    boxes = np.concatenate((xy-wh/np.float32(2),xy+wh/np.float32(2)),axis=1)
    order = np.lexsort((classes,anchors,-s))[:30000]
    keep = []
    while order.size and len(keep)<300:
        k=int(order[0]); keep.append(k); remaining=order[1:]
        if not remaining.size: break
        b=boxes[k]; q=boxes[remaining]
        inter=np.maximum(0,np.minimum(b[2:],q[:,2:])-np.maximum(b[:2],q[:,:2])).prod(axis=1)
        area=(b[2]-b[0])*(b[3]-b[1]); other=(q[:,2]-q[:,0])*(q[:,3]-q[:,1])
        denom=area+other-inter
        iou=np.divide(inter,denom,out=np.zeros_like(inter),where=denom>0)
        order=remaining[~((classes[remaining]==classes[k]) & (iou>np.float32(.7)))]
    result=[]
    for k in keep:
        b=boxes[k].copy(); b[[0,2]]-=np.float32(im['left']); b[[1,3]]-=np.float32(im['top'])
        b/=np.float32(im['scale']); b[[0,2]]=b[[0,2]].clip(0,im['width']); b[[1,3]]=b[[1,3]].clip(0,im['height'])
        result.append([int(anchors[k]),int(classes[k]),float(s[k]),*map(float,b)])
    return result


def coco_evaluate(folder, detections, name):
    from pycocotools.coco import COCO
    from pycocotools.cocoeval import COCOeval
    gt=COCO(str(folder/'instances_subset.json'))
    categories=sorted(gt.getCatIds()); predictions=[]
    if len(categories)!=80: raise ValueError('EXPECTED_80_COCO_CATEGORIES')
    for row in detections:
        if row['image_id'] not in gt.imgs: continue
        for _,c,s,l,t,r,b in row['evaluation']:
            predictions.append({'image_id':row['image_id'],'category_id':categories[c],
                                'score':s,'bbox':[l,t,r-l,b-t]})
    if not predictions: raise ValueError('NO_COCO_PREDICTIONS')
    save(folder/f'{name}-coco-predictions.json',predictions)
    dt=gt.loadRes(predictions)
    evaluator=COCOeval(gt,dt,'bbox'); evaluator.params.imgIds=sorted(gt.imgs)
    evaluator.evaluate(); evaluator.accumulate(); evaluator.summarize()
    metrics={'images':len(gt.imgs),'predictions':len(predictions),'stats':evaluator.stats.tolist(),
             'AP':float(evaluator.stats[0]),'AP50':float(evaluator.stats[1]),'AP75':float(evaluator.stats[2])}
    save(folder/f'{name}-metrics.json',metrics)
    return metrics


def reference(args):
    import onnxruntime as ort
    contract=json.loads(args.contract.read_text(encoding='utf-8'))
    if sha(args.onnx)!=contract['onnx_sha256']: raise ValueError('ONNX_HASH_MISMATCH')
    out=args.folder/'reference'; out.mkdir(exist_ok=False)
    options=ort.SessionOptions(); options.intra_op_num_threads=2; options.inter_op_num_threads=1
    session=ort.InferenceSession(str(args.onnx),sess_options=options,providers=['CPUExecutionProvider'])
    if session.get_inputs()[0].shape!=[1,3,640,640] or session.get_outputs()[0].shape!=[1,84,8400]:
        raise ValueError('ONNX_IO_MISMATCH')
    manifest=json.loads((args.folder/'input-manifest.json').read_text())
    results=[]
    with (out/'detections.jsonl').open('w',encoding='utf-8') as stream:
        for im in manifest['records']:
            p=args.folder/'corpus'/f"{im['image_id']}.u8"
            if sha(p)!=im['u8_sha256']: raise ValueError('CORPUS_CHANGED')
            x=np.frombuffer(p.read_bytes(),np.uint8).astype(np.float32).reshape(1,3,640,640)/np.float32(255)
            if sha_bytes(x.tobytes())!=im['f32_sha256']: raise ValueError('NORMALIZATION_CHANGED')
            y=session.run(['output0'],{'images':x})[0]
            if im['fixed']: (out/f"{im['image_id']}.f32").write_bytes(y.tobytes())
            row={'image_id':im['image_id'],'display':decode(y,im), 'evaluation':decode(y,im,.001,True)}
            stream.write(json.dumps(row,allow_nan=False)+'\n'); stream.flush(); results.append(row)
            print(f"reference completed {im['image_id']} display={len(row['display'])}",flush=True)
    metrics=coco_evaluate(args.folder,results,'reference')
    save(out/'summary.json',{'images':len(results),'onnx_sha256':sha(args.onnx),'contract_sha256':sha(args.contract),
                             'onnxruntime_version':ort.__version__,'providers':session.get_providers(),
                             'numpy_version':np.__version__,'opencv_version':cv2.__version__,'metrics':metrics})


def read_results(path):
    values=[json.loads(line) for line in path.read_text().splitlines()]
    indexed={i['image_id']:i for i in values}
    if len(values)!=len(indexed): raise ValueError('DUPLICATE_RESULT')
    return indexed


def pair_iou(a,b):
    l,t,r,d=a[3:]; ll,tt,rr,dd=b[3:]
    inter=max(0,min(r,rr)-max(l,ll))*max(0,min(d,dd)-max(t,tt))
    area=(r-l)*(d-t)+(rr-ll)*(dd-tt)-inter
    return inter/area if area>0 else 0


def compare(args):
    manifest=json.loads((args.folder/'input-manifest.json').read_text())
    ref=read_results(args.folder/'reference/detections.jsonl')
    target=read_results(args.target/'detections.jsonl')
    if set(target)!=set(ref): raise ValueError('RESULT_IMAGE_SET_MISMATCH')
    contract=json.loads(args.contract.read_text(encoding='utf-8')); gates=contract['first_evaluation_gates']
    errors=[]; parse_failures=[]; matched=expected=actual=0; unmatched=[]
    for im in manifest['records']:
        iid=im['image_id']
        if not im['fixed']: continue
        a=np.fromfile(args.folder/f'reference/{iid}.f32',np.float32).reshape(84,8400)
        b=np.fromfile(args.target/f'{iid}.f32',np.float32).reshape(84,8400)
        if not np.isfinite(b).all(): raise ValueError('NONFINITE_TARGET')
        for field,sl in [('coordinates',slice(0,4)),('scores',slice(4,84))]:
            delta=np.abs(a[sl].astype(np.float64)-b[sl])
            errors.append({'image_id':iid,'field':field,'max_abs':float(delta.max()),'p99_abs':float(np.quantile(delta,.99)),
                           'rms':float(np.sqrt((delta**2).mean()))})
        for field,conf,multi in [('display',.25,False),('evaluation',.001,True)]:
            independent=decode(b,im,conf,multi); cpp=target[iid][field]
            same=len(independent)==len(cpp) and all(v[:2]==w[:2] and np.allclose(v[2:],w[2:],rtol=0,atol=.001)
                                                       for v,w in zip(independent,cpp))
            if not same: parse_failures.append({'image_id':iid,'field':field,'numpy_count':len(independent),'cpp_count':len(cpp)})
        aa=ref[iid]['display']; bb=target[iid]['display']; used=set()
        expected+=len(aa); actual+=len(bb)
        for box in aa:
            options=[(pair_iou(box,q),n) for n,q in enumerate(bb) if n not in used and q[1]==box[1]]
            overlap,n=max(options,default=(0,-1))
            if overlap>=gates['fixed_display_match_iou_minimum'] and abs(box[2]-bb[n][2])<=gates['fixed_display_score_absolute_error_maximum']:
                matched+=1; used.add(n)
            else: unmatched.append({'image_id':iid,'reference_box':box,'best_iou':overlap})
    metrics=coco_evaluate(args.folder,list(target.values()),args.target.name)
    reference_metrics=json.loads((args.folder/'reference-metrics.json').read_text())
    drop=reference_metrics['AP']-metrics['AP']; fraction=matched/max(expected,actual,1)
    report={'target':str(args.target),'contract_sha256':sha(args.contract),'reference_AP':reference_metrics['AP'],
            'target_AP':metrics['AP'],'absolute_AP_drop':drop,'fixed_matched':matched,'fixed_reference_detections':expected,
            'fixed_target_detections':actual,'fixed_match_fraction':fraction,'parser_failures':parse_failures,
            'unmatched':unmatched,'raw_errors':errors,
            'pass':not parse_failures and drop<=gates['fp16_max_absolute_AP_drop'] and fraction>=gates['fixed_display_minimum_match_fraction']}
    save(args.folder/f'{args.target.name}-comparison.json',report)
    print(json.dumps({k:v for k,v in report.items() if k not in ('raw_errors','unmatched')},indent=2))
    if not report['pass']: raise SystemExit(1)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='mode',required=True)
    p=sub.add_parser('prepare'); p.add_argument('--package',type=pathlib.Path,required=True)
    p.add_argument('--package-sha256',required=True); p.add_argument('--out',type=pathlib.Path,required=True)
    p=sub.add_parser('reference'); p.add_argument('--folder',type=pathlib.Path,required=True)
    p.add_argument('--onnx',type=pathlib.Path,required=True); p.add_argument('--contract',type=pathlib.Path,required=True)
    p=sub.add_parser('compare'); p.add_argument('--folder',type=pathlib.Path,required=True)
    p.add_argument('--target',type=pathlib.Path,required=True); p.add_argument('--contract',type=pathlib.Path,required=True)
    args=parser.parse_args(); globals()[args.mode](args)


if __name__=='__main__': main()
