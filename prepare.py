from pathlib import Path
import hashlib,json,zipfile
root=Path(__file__).parent;payload=root/'payload';build=root/'build';build.mkdir(exist_ok=True)
for f in root.rglob('*.ps1'):
    data=f.read_text(encoding='utf-8-sig').replace('\r\n','\n')
    f.write_bytes(b'\xef\xbb\xbf'+data.replace('\n','\r\n').encode('utf-8'))
files=[]
for folder in ['scripts','bin','config']:
    for f in sorted((payload/folder).iterdir()):
        if f.is_file():files.append({'Path':f.relative_to(payload).as_posix(),'SHA256':hashlib.sha256(f.read_bytes()).hexdigest().upper()})
(payload/'manifest.json').write_text(json.dumps({'Version':'0.3.1-rc1','Files':files},indent=2),encoding='utf-8')
zpath=build/'payload.zip'
with zipfile.ZipFile(zpath,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for f in sorted(payload.rglob('*')):
        if f.is_file() and 'reports' not in f.relative_to(payload).parts:z.write(f,f.relative_to(payload).as_posix())
digest=hashlib.sha256(zpath.read_bytes()).hexdigest().upper()
(build/'BuildInfo.cs').write_text('internal static class BuildInfo { public const string PayloadHash="'+digest+'"; }\n',encoding='utf-8')
print('Embedded payload:',zpath.stat().st_size,'bytes; files=',len(files),'; SHA256=',digest)
