#!/usr/bin/env python3
"""Build the M4 GPL trace patch; DOES NOT install or switch any module."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import zipfile
from build_uvc_module import sha, command


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-record',type=pathlib.Path,required=True)
    parser.add_argument('--package',type=pathlib.Path,required=True)
    parser.add_argument('--patch',type=pathlib.Path,required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    args=parser.parse_args()
    base=json.loads(args.baseline_record.read_text())
    if os.uname().machine!='aarch64' or os.uname().release!=base['release']:
        raise RuntimeError('UNVERIFIED_TARGET')
    if base['build_exit_code'] or not base['headers_unchanged'] or not base['vermagic']['equal'] or not base['srcversion']['equal']:
        raise RuntimeError('UNMODIFIED_BASELINE_REQUIRED')
    root=pathlib.Path(__file__).resolve().parents[3]
    args.out=args.out.resolve(); args.out.relative_to(root/'projects/edgevision/artifacts')
    args.out.mkdir(parents=True,exist_ok=False)
    headers=pathlib.Path(base['headers'])
    if sha(headers/'.config')!=base['config_sha256'] or sha(headers/'Module.symvers')!=base['symvers_sha256']:
        raise RuntimeError('KERNEL_HEADERS_CHANGED')
    if sha(args.package)!=base['package_sha256']: raise RuntimeError('PRISTINE_PACKAGE_CHANGED')
    record={'release':base['release'],'headers':str(headers),'config_sha256':base['config_sha256'],
            'symvers_sha256':base['symvers_sha256'],'pristine_package_sha256':sha(args.package),
            'patch_sha256':sha(args.patch),'baseline_record_sha256':sha(args.baseline_record),
            'compiler':command(['gcc','--version']),'installed':False}
    try:
        with zipfile.ZipFile(args.package) as z:
            source=json.loads(z.read('uvc-sources.json')); record['source']=source
            for item in source['files']:
                name=item['name']
                if pathlib.PurePosixPath(name).name!=name: raise ValueError('INVALID_SOURCE_NAME')
                data=z.read('uvc-pristine/'+name)
                if hashlib.sha256(data).hexdigest()!=item['sha256']: raise ValueError('SOURCE_HASH_MISMATCH')
                (args.out/name).write_bytes(data)
        patch=subprocess.run(['patch','--batch','--forward','--fuzz=0','-p5','-i',str(args.patch.resolve())],
                             cwd=args.out,text=True,capture_output=True)
        record['patch_application']={'exit_code':patch.returncode,'stdout':patch.stdout,'stderr':patch.stderr}
        if patch.returncode: raise RuntimeError('PATCH_APPLICATION_FAILED')
        record['modified_sources']=[{'path':p.name,'sha256':sha(p)} for p in sorted(args.out.iterdir()) if p.is_file()]
        argv=['make','-C',str(headers),'M='+str(args.out),'ARCH=arm64','CROSS_COMPILE=','CC=gcc','-j2','modules']
        with (args.out/'build.log').open('wb') as log:
            built=subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT)
        record['build_argv']=argv; record['build_exit_code']=built.returncode
        record['headers_unchanged']=sha(headers/'.config')==base['config_sha256'] and sha(headers/'Module.symvers')==base['symvers_sha256']
        if built.returncode: raise RuntimeError('MODULE_BUILD_FAILED')
        module=args.out/'uvcvideo.ko'; record['built_module_sha256']=sha(module)
        record['built_modinfo']=command(['modinfo',str(module)])
        info=lambda name:subprocess.check_output(['modinfo','-F',name,str(module)],text=True).strip()
        record['vermagic']=info('vermagic'); record['srcversion']=info('srcversion')
        versions=command(['modprobe','--show-modversions',str(module)])
        if versions['exit_code']: raise RuntimeError('MODVERSIONS_UNREADABLE')
        expected={parts[1]:parts[0] for parts in (line.split() for line in (headers/'Module.symvers').read_text().splitlines()) if len(parts)>1}
        actual={line.split()[1]:line.split()[0] for line in versions['stdout'].splitlines()}
        record['imported_crc']={'count':len(actual),'mismatches':{k:{'expected':expected.get(k),'actual':v} for k,v in actual.items() if expected.get(k)!=v}}
        if record['vermagic']!=base['vermagic']['original'] or record['imported_crc']['mismatches'] or not record['headers_unchanged']:
            raise RuntimeError('BUILT_MODULE_ABI_MISMATCH')
    except BaseException as e:
        record['failure']=str(e); raise
    finally:
        (args.out/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'build_exit_code':0,'module_sha256':record['built_module_sha256'],
                      'srcversion':record['srcversion'],'imported_crc':record['imported_crc']}))


if __name__=='__main__': main()
