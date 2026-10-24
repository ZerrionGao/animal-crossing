#!/usr/bin/env python3
"""Compile the verified startup MSL seed into device metallibs and refresh its manifest."""
import argparse, concurrent.futures, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path

# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic. The
# offline compiler defaults to fast math ("Default is 'fast'"), which breaks their float integer-division idioms.
METAL_FLAGS=['-fmetal-math-mode=safe']
ACNH_POLICY='acnh-metal-v1'

def policy(normalizer):
  if normalizer is None: return None, METAL_FLAGS
  normalizer=Path(normalizer).expanduser()
  if not normalizer.is_absolute(): raise SystemExit('--acnh-normalizer must be an absolute path')
  normalizer=normalizer.resolve()
  result=subprocess.run([str(normalizer),f'--policy-id={ACNH_POLICY}','--metal-flags'],check=True,capture_output=True,text=True)
  flags=result.stdout.strip().split()
  if flags != ['-fmetal-math-mode=safe','-fpreserve-invariance']:
    raise SystemExit('ACNH normalizer returned unexpected Metal flags')
  return normalizer,flags

def main():
  ap=argparse.ArgumentParser()
  ap.add_argument('seed',type=Path);ap.add_argument('sdk');ap.add_argument('--acnh-normalizer',type=Path)
  a=ap.parse_args();seed=a.seed.resolve();sdk=a.sdk
  normalizer,metal_flags=policy(a.acnh_normalizer)
  manifest=json.loads((seed/'seed-manifest.json').read_text())
  if manifest.get('chain') is None or not manifest.get('files'): raise SystemExit('invalid shader seed manifest')
  files=sorted(n for n in manifest['files'] if n.endswith('_b.metal'))
  def compile_one(name):
    src=seed/name; air=seed/(name+'.air'); lib=seed/(name.removesuffix('.metal')+'.metallib')
    compile_src=src
    with tempfile.TemporaryDirectory(prefix='acnh-msl-') as temp:
      if normalizer:
        compile_src=Path(temp)/src.name
        subprocess.run([str(normalizer),f'--policy-id={ACNH_POLICY}',str(src),str(compile_src)],check=True,capture_output=True)
      subprocess.run(['xcrun','-sdk',sdk,'metal',*metal_flags,'-c',str(compile_src),'-o',str(air)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    subprocess.run(['xcrun','-sdk',sdk,'metallib',str(air),'-o',str(lib)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    air.unlink()
    return lib.name,hashlib.sha256(lib.read_bytes()).hexdigest()
  results=[]
  with concurrent.futures.ThreadPoolExecutor(max_workers=min(8,os.cpu_count() or 4)) as pool:
    for i,result in enumerate(pool.map(compile_one,files),1):
        results.append(result)
        if i%100==0: print(f'compiled {i}/{len(files)} iOS metallibs',flush=True)
  for name,digest in results: manifest['files'][name]=digest
  manifest['ios_metallibs']=len(results)
  manifest['ios_metal_flags']=' '.join(metal_flags)   # build_ipa.py reuses the libraries only when these match
  if normalizer: manifest['shader_policy']=ACNH_POLICY
  manifest['files']={n:hashlib.sha256((seed/n).read_bytes()).hexdigest() for n in sorted(manifest['files'])}
  (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  print(f'compiled {len(results)} iOS metallibs')

if __name__=='__main__': main()
