"""Build a reproducible SD model pack for Chakshu from pinned Espressif weights."""
import hashlib, json, pathlib, struct, zipfile, sys, subprocess
REV = '27da4f945f779bab2d238889924622f7988b1b1c'
FILES = {
 'mn5q8_en': {'_MODEL_INFO_':'2488263ce5dd4d27a50d07604792e233f2c248c6','mn5q8_data':'ccafc8b30bc5cd6cb8cc103cccc943959fc688eb','mn5q8_index':'f17c77e331bd51e88d566db7f80aacb821bef153'},
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
            actual=hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
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
    source=(pathlib.Path(__file__).resolve().parent.parent/'firmware/xiao-sense/voice.cpp').read_text()
    if f'MODEL_BYTES={len(packed)};' not in source or f'MODEL_SHA256[]="{digest}"' not in source:
        raise ValueError('Model pack does not match firmware integrity constants')
    manifest={'schema':1,'source':REV,'models':list(FILES),'bytes':len(packed),'sha256':digest}
    (out/'model.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'srmodels.bin').write_bytes(packed)
    license=download('LICENSE')
    with zipfile.ZipFile(out/'chakshu-voice-model.zip','w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('synap/models/srmodels.bin',packed)
        z.writestr('synap/models/model.json',json.dumps(manifest,indent=2)+'\n')
        z.writestr('ESPRESSIF-LICENSE.txt',license)
        z.writestr('README.txt','Copy the synap folder to the root of the Chakshu SD card, preserving your existing recordings. Reinsert the card and restart Chakshu. Say Hi Chakshu, pause, then take photo / start video / stop video / audio on / audio off. Recognition is local; audio off stops recording, not the command listener. Disable Voice controls in Synap settings to stop listening for commands. Model weights: '+REV+'\n')
    print(json.dumps(manifest))
if __name__=='__main__':build(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'voice-model'))
