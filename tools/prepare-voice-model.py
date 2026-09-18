"""Build a reproducible WakeNet-only Chakshu diagnostic model pack."""
import hashlib, json, pathlib, struct, zipfile, sys, subprocess
REV = '27da4f945f779bab2d238889924622f7988b1b1c'
FILES = {
 'wn9_hiesp': {
   '_MODEL_INFO_':'0373ecf1e9f2f8fbb6ad168adee3d87849b8ad71',
   'wn9_data':'7d99255f8c8f82cdbacad7e065fd63a9be1e3c0a',
   'wn9_index':'3845b374a1b96d54d5c1574f6456403ea45f9f81',
 },
}
def download(path):
    url=f'https://raw.githubusercontent.com/espressif/esp-sr/{REV}/{path}'
    return subprocess.check_output(['curl','--fail','--location','--silent','--show-error','--max-time','30',url])
def build(out):
    out.mkdir(parents=True, exist_ok=True)
    entries=[]
    for model, files in FILES.items():
        family='multinet_model' if model.startswith('mn') else 'wakenet_model'
        group=[]
        for name, expected in files.items():
            data=download(f'model/{family}/{model}/{name}')
            actual=hashlib.sha1(f'blob {len(data)}\\0'.encode()+data).hexdigest()
            if actual!=expected: raise ValueError(f'Unexpected model content: {model}/{name}')
            group.append((name,data))
        entries.append((model,group))
    offset=4+len(entries)*36+sum(len(files)*40 for _,files in entries)
    header=struct.pack('<I',len(entries)); body=b''
    for model,files in entries:
        header+=struct.pack('<32sI',model.encode(),len(files))
        for name,data in files:
            header+=struct.pack('<32sII',name.encode(),offset+len(body),len(data)); body+=data
    packed=header+body
    digest=hashlib.sha256(packed).hexdigest()
    manifest={'schema':1,'purpose':'wakenet-diagnostic','source':REV,'models':list(FILES),'bytes':len(packed),'sha256':digest}
    (out/'model.json').write_text(json.dumps(manifest,indent=2)+'\\n')
    (out/'srmodels.bin').write_bytes(packed)
    license=download('LICENSE')
    (out/'ESPRESSIF-LICENSE.txt').write_bytes(license)
    with zipfile.ZipFile(out/'chakshu-voice-model.zip','w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('synap/models/srmodels.bin',packed)
        z.writestr('synap/models/model.json',json.dumps(manifest,indent=2)+'\\n')
        z.writestr('ESPRESSIF-LICENSE.txt',license)
        z.writestr('README.txt','Temporary Synap Chakshu WakeNet diagnostic. Say Hi ESP and verify the orange wake acknowledgement. MultiNet command recognition is intentionally absent from this diagnostic OTA. Model weights: '+REV+'\\n')
    print(json.dumps(manifest))
if __name__=='__main__':build(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'voice-model'))
