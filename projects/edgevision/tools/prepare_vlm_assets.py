#!/usr/bin/env python3
"""Prepare owned links/configuration without modifying the old model assets.

Weights and engines are linked read-only by convention, never copied into Git.
The derived configuration is a new runtime experiment, not an engine re-export.
"""
import argparse
import copy
import hashlib
import json
import pathlib


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--donor',type=pathlib.Path,required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    parser.add_argument('--profile',choices=['original','reduced-candidate'],default='original',
        help='original preserves the validated engine profile; reduced-candidate is unaccepted and may require engine rebuild')
    args=parser.parse_args();donor=args.donor.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    original=json.loads((donor/'engines/llm/config.json').read_text())
    assert original['model']=='qwen3_vl_text' and original['builder_config']['max_batch_size']==1
    adjusted=copy.deepcopy(original)
    overrides={'max_input_len':1024,'max_kv_cache_capacity':1024,'max_kv_pool_pages':8} if args.profile=='reduced-candidate' else {}
    for key,value in overrides.items():
        assert value<=original['builder_config'][key]
        adjusted['builder_config'][key]=value
    inputs=[];excluded=[]
    for name in ['llm','visual']:
        folder=out/name;folder.mkdir()
        for source in sorted((donor/'engines'/name).iterdir()):
            if name=='visual' and source.name=='action':
                # The donor action.engine is a symlink to visual.engine, not an
                # Alpamayo action expert. Loading it creates a second visual
                # context before config parsing fails. This project uses VQA
                # and registered tools; no pretrained action expert is needed.
                excluded.append({'donor_path':str(source),'reason':'optional action expert not used by this VQA/tool adapter'})
                continue
            if name=='llm' and source.name=='config.json' and overrides:
                (folder/source.name).write_text(json.dumps(adjusted,indent=2)+'\n')
            elif source.name=='config.json':
                (folder/source.name).write_bytes(source.read_bytes())
            else:
                (folder/source.name).symlink_to(source.resolve(),target_is_directory=source.is_dir())
            if source.is_file():inputs.append({'donor_path':str(source),'bytes':source.stat().st_size,'sha256':sha(source)})
    manifest={'schema_version':1,'model':'Qwen3-VL-2B-Instruct','hf_revision':'89644892e4d85e24eaac8bacfd4f463576704203',
        'source_weight_sha256':'7de1838c87a5349b016c26a1c3f7d2bc400a3d485f95ef39a7059ffd734977a0',
        'donor_read_only':True,'engine_rebuilt':False,'profile':args.profile,'runtime_overrides':overrides,'inputs':inputs,
        'excluded_optional_assets':excluded,
        'derived_llm_config_sha256':sha(out/'llm/config.json'),
        'acceptance':'requires fresh independent VQA/planner and combined runtime memory tests; not accepted by configuration alone'}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'out':str(out),'derived_llm_config_sha256':manifest['derived_llm_config_sha256'],'runtime_overrides':overrides}))


if __name__=='__main__':main()
