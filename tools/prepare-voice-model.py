"""Build a reproducible Chakshu WakeNet + MultiNet model pack."""
import hashlib, json, pathlib, struct, zipfile, sys, subprocess

REV = '27da4f945f779bab2d238889924622f7988b1b1c'
FILES = {
 'wn9s_hiesp': {
   '_MODEL_INFO_':'f1356d7a9d138464ac7a6a20b42a9d020df65e34',
   'wn9_data':'9b0c90d6ce4c3160f08d7ddfb2cd9935e702f9ee',
   'wn9_index':'e52727fbeeddd00fddee1eaf5e435253976f041f',
 },
 'mn5q8_en': {
   '_MODEL_INFO_':'2488263ce5dd4d27a50d07604792e233f2c248c6',
   'mn5q8_data':'ccafc8b30bc5cd6cb8cc103cccc943959fc688eb',
   'mn5q8_index':'f17c77e331bd51e88d566db7f80aacb821bef153',
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
            actual=hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
            if actual!=expected:
                raise ValueError(f'Unexpected model content: {model}/{name}')
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
    manifest={
      'schema':1,
      'purpose':'wakenet-multinet-production',
      'source':REV,
      'models':list(FILES),
      'bytes':len(packed),
      'sha256':digest,
      'wake_word':'Hi ESP',
      'command_window_ms':8000,
    }
    (out/'model.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'srmodels.bin').write_bytes(packed)
    license=download('LICENSE')
    (out/'ESPRESSIF-LICENSE.txt').write_bytes(license)
    with zipfile.ZipFile(out/'chakshu-voice-model.zip','w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('synap/models/srmodels.bin',packed)
        z.writestr('synap/models/model.json',json.dumps(manifest,indent=2)+'\n')
        z.writestr('ESPRESSIF-LICENSE.txt',license)
        z.writestr('README.txt','Synap Chakshu local voice pack. Say Hi ESP, then a supported command within 8 seconds. WakeNet handles wake detection and MultiNet handles commands. Model weights: '+REV+'\n')
    print(json.dumps(manifest))

if __name__=='__main__':
    build(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'voice-model'))
