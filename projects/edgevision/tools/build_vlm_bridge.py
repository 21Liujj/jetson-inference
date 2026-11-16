#!/usr/bin/env python3
"""Link an owned JSONL adapter against the audited existing EdgeQwen SDK.

The donor device-link object is reused explicitly and hashed. Donor source/build
outputs are never modified; TensorRT-Edge-LLM is not a separate Git checkout there.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--donor',type=pathlib.Path,required=True)
    parser.add_argument('--source',type=pathlib.Path,required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    args=parser.parse_args();donor=args.donor.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    sdk=donor/'examples/runtime_sdk';build=donor/'build';cuda=pathlib.Path('/usr/local/cuda/targets/aarch64-linux/lib')
    libraries=[build/'examples/runtime_sdk/libedgeqwen_runtime.a',build/'cpp/libedgellmCore.a',
               donor/'cpp/kernels/cuteDSLArtifact/aarch64/sm_87/libcutedsl_aarch64.a',
               build/'cpp/libtrt_edgellm_cutedsl_cudart_shim.a']
    device_link=build/'examples/runtime_sdk/CMakeFiles/qwen_ask.dir/cmake_device_link.o'
    inputs=[args.source.resolve(),sdk/'include/edgeqwen/runtime.h',device_link,*libraries]
    manifest={'schema_version':1,'donor_read_only':True,'donor_local_repository_head':
              subprocess.check_output(['git','rev-parse','HEAD'],cwd=donor,text=True).strip(),
              'donor_git_toplevel':subprocess.check_output(['git','rev-parse','--show-toplevel'],cwd=donor,text=True).strip(),
              'inputs':[{'path':str(p),'bytes':p.stat().st_size,'sha256':sha(p)} for p in inputs]}
    argv=['/usr/bin/g++','-std=c++17','-O2','-Wall','-Wextra','-I'+str(sdk/'include'),str(args.source.resolve()),
          str(device_link),'-o',str(out/'edgevision_vlm_bridge'),'-L'+str(cuda),'-L'+str(cuda/'stubs'),
          '-Wl,-rpath,'+str(cuda),'-Wl,--unresolved-symbols=ignore-in-shared-libs',*map(str,libraries),
          '-lcuda','-lnvonnxparser','-lnvinfer','-lcudart','-lcudadevrt','-lcudart_static','-lrt','-lpthread','-ldl']
    manifest['argv']=argv
    with (out/'build.log').open('wb') as log:
        run=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=180)
    manifest['exit_code']=run.returncode
    if not run.returncode:manifest['binary_sha256']=sha(out/'edgevision_vlm_bridge')
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'exit_code':run.returncode,'out':str(out)}))
    raise SystemExit(run.returncode)


if __name__=='__main__':main()
