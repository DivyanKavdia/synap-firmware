#!/usr/bin/env python3
"""Train the bootstrap Chakshu TinyML v1 keyword classifier.

This is an offline developer tool, not a firmware build dependency.
Requires: espeak, numpy, scipy, scikit-learn, torch.

Classes: noise, unknown, hey_snap, photo, video, stop.
The generated header contains int8 weights and float normalization/bias constants.
"""
from __future__ import annotations
import argparse, math, os, random, subprocess, tempfile
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score
import torch
import torch.nn as nn

SEED=7
FS=16000
WINDOW=15360
TIME=20
FRAME=512
HOP=768
BINS=np.array([8,12,18,26,38,54,76,106,140,180,220,248],dtype=np.int64)
LABELS=["noise","unknown","hey_snap","photo","video","stop"]
PHRASES={
 "hey_snap":["hey snap"],
 "photo":["take a photo","take a snap","take photo"],
 "video":["record a video","take a video","record video"],
 "stop":["stop","stop recording","stop video","stop audio"],
 "unknown":["hello there","good morning","what time is it","turn on the light","how are you",
            "play music","call home","open settings","battery status","thank you","yes please","no thanks",
            "tell me a joke","where are we","start now","take me home","weather today","read my messages",
            "volume up","volume down","hey there","snap a finger","record this","take it","what is this",
            "camera please"],
}
VOICES=["en","en-us","en-sc","en-uk-north","en-uk-rp","en-wi"]

def speak(text,voice,speed,pitch,amp):
    fd,path=tempfile.mkstemp(suffix=".wav");os.close(fd)
    try:
        subprocess.run(["espeak","-v",voice,"-s",str(speed),"-p",str(pitch),"-a",str(amp),"-w",path,text],
                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
        sr,x=wavfile.read(path)
    finally:
        try: os.unlink(path)
        except OSError: pass
    if x.ndim>1:x=x.mean(1)
    x=x.astype(np.float32)
    if sr!=FS:x=resample_poly(x,FS,sr).astype(np.float32)
    peak=float(np.max(np.abs(x))) or 1.0
    return x/peak

def augment(x,rng):
    x=x.copy()*rng.uniform(.30,1.0)
    x+=rng.normal(0,rng.uniform(.001,.018),len(x)).astype(np.float32)
    if rng.random()<.5:
        t=np.arange(len(x),dtype=np.float32)/FS
        x+=(rng.uniform(.001,.012)*np.sin(2*np.pi*rng.choice([50,60,100,120,240,400])*t)).astype(np.float32)
    if rng.random()<.25:x=np.tanh(x*rng.uniform(1.0,1.8)).astype(np.float32)
    out=np.zeros(WINDOW,np.float32)
    if len(x)>WINDOW:
        starts=[0,max(0,(len(x)-WINDOW)//2),max(0,len(x)-WINDOW)]
        start=int(rng.choice(starts)) if rng.random()<.75 else int(rng.integers(0,len(x)-WINDOW+1))
        x=x[start:start+WINDOW]
    start=int(rng.integers(0,max(1,WINDOW-len(x)+1))) if len(x)<WINDOW else 0
    out[start:start+min(len(x),WINDOW)]=x[:WINDOW-start]
    rms=float(np.sqrt(np.mean(out*out))+1e-6)
    if rms>.004:out*=min(10.0,.10/rms)
    return np.clip(out,-.95,.95)

def noise_sample(rng):
    t=np.arange(WINDOW,dtype=np.float32)/FS
    x=rng.normal(0,rng.uniform(.0002,.035),WINDOW).astype(np.float32)
    if rng.random()<.7:x+=(rng.uniform(0,.025)*np.sin(2*np.pi*rng.uniform(40,1200)*t)).astype(np.float32)
    if rng.random()<.35:
        start=int(rng.integers(0,WINDOW-800));length=int(rng.integers(300,2400))
        x[start:min(WINDOW,start+length)]+=rng.normal(0,rng.uniform(.02,.09),min(length,WINDOW-start))
    return np.clip(x,-1,1)

def features(x):
    rows=[]
    for i in range(TIME):
        start=i*HOP
        frame=x[start:start+FRAME].astype(np.float64)
        if len(frame)<FRAME:frame=np.pad(frame,(0,FRAME-len(frame)))
        frame-=frame.mean()
        fft=np.fft.rfft(frame)
        power=(fft.real*fft.real+fft.imag*fft.imag)[BINS]/(FRAME*FRAME)
        rows.append(np.log1p(power*1e5))
    return np.asarray(rows,np.float32)

def dataset(count):
    rng=np.random.default_rng(SEED);cache={};x=[];y=[]
    for label_id,label in enumerate(LABELS):
        for _ in range(count):
            if label=="noise":audio=noise_sample(rng)
            else:
                phrase=random.choice(PHRASES[label]);voice=random.choice(VOICES)
                key=(phrase,voice,random.randrange(125,201,5),random.randrange(30,81,5),random.randrange(120,201,10))
                if key not in cache:cache[key]=speak(*key)
                audio=augment(cache[key],rng)
            x.append(features(audio));y.append(label_id)
    return np.stack(x),np.asarray(y)

class Net(nn.Module):
    def __init__(self):
        super().__init__()
        self.c1=nn.Conv1d(12,16,3,padding=1)
        self.c2=nn.Conv1d(16,16,3,padding=1)
        self.fc=nn.Linear(32,6)
    def forward(self,x):
        x=x.transpose(1,2)
        x=torch.relu(self.c1(x));x=torch.relu(self.c2(x))
        return self.fc(torch.cat([x.amax(2),x.mean(2)],1))

def quantize(weight):
    array=weight.detach().cpu().numpy()
    scale=float(np.max(np.abs(array))/127 or 1)
    return np.round(array/scale).clip(-127,127).astype(np.int8),scale

def c_array(name,array,ctype,per=16,float_values=False):
    flat=np.asarray(array).reshape(-1)
    values=[f"{float(v):.9g}f" if float_values else str(int(v)) for v in flat]
    lines=["  "+", ".join(values[i:i+per]) for i in range(0,len(values),per)]
    return f"constexpr {ctype} {name}[{len(values)}] = {{\n"+",\n".join(lines)+"\n};\n"

def emit_header(path,net,mean,invstd,accuracy):
    q1,s1=quantize(net.c1.weight);q2,s2=quantize(net.c2.weight);qf,sf=quantize(net.fc.weight)
    coeff=np.array([2*math.cos(2*math.pi*int(b)/FRAME) for b in BINS],np.float32)
    out=f"""// Generated bootstrap TinyML model for Chakshu local voice v1.
// Architecture: 12 spectral bins x 20 time frames -> Conv16 -> Conv16 -> max+mean pool -> 6 classes.
// Learned weights: 1,536 bytes int8. Synthetic held-out quantized accuracy: {accuracy*100:.1f}%.
// Bootstrap-only: real pendant utterances should replace/augment synthetic training data.
#pragma once
#include <stdint.h>
namespace ChakshuTinyModel {{
constexpr uint16_t SAMPLE_RATE=16000;
constexpr uint16_t WINDOW_SAMPLES=15360;
constexpr uint16_t FRAME_SAMPLES=512;
constexpr uint16_t FRAME_HOP=768;
constexpr uint8_t TIME_FRAMES=20;
constexpr uint8_t BANDS=12;
constexpr uint8_t CHANNELS=16;
constexpr uint8_t CLASSES=6;
enum Class : uint8_t {{ NOISE=0, UNKNOWN=1, HEY_SNAP=2, PHOTO=3, VIDEO=4, STOP=5 }};
"""
    out+=c_array("GOERTZEL_COEFF",coeff,"float",6,True)
    out+=c_array("FEATURE_MEAN",mean,"float",6,True)
    out+=c_array("FEATURE_INV_STD",invstd,"float",6,True)
    out+=f"constexpr float C1_SCALE={s1:.9g}f;\n"+c_array("C1_WEIGHT",q1,"int8_t",24)+c_array("C1_BIAS",net.c1.bias.detach().numpy(),"float",8,True)
    out+=f"constexpr float C2_SCALE={s2:.9g}f;\n"+c_array("C2_WEIGHT",q2,"int8_t",24)+c_array("C2_BIAS",net.c2.bias.detach().numpy(),"float",8,True)
    out+=f"constexpr float FC_SCALE={sf:.9g}f;\n"+c_array("FC_WEIGHT",qf,"int8_t",24)+c_array("FC_BIAS",net.fc.bias.detach().numpy(),"float",6,True)
    out+="constexpr uint32_t LEARNED_WEIGHT_BYTES=sizeof(C1_WEIGHT)+sizeof(C2_WEIGHT)+sizeof(FC_WEIGHT);\n}\n"
    Path(path).write_text(out)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--count",type=int,default=220,help="synthetic examples per class")
    parser.add_argument("--out",default="firmware/xiao-sense/tiny-voice-model.h")
    args=parser.parse_args()
    random.seed(SEED);np.random.seed(SEED);torch.manual_seed(SEED)
    x,y=dataset(args.count)
    x_train,x_test,y_train,y_test=train_test_split(x,y,test_size=.25,random_state=SEED,stratify=y)
    mean=x_train.mean(axis=(0,1));std=x_train.std(axis=(0,1))+1e-3
    x_train=(x_train-mean)/std;x_test=(x_test-mean)/std
    net=Net();optimizer=torch.optim.Adam(net.parameters(),lr=.006,weight_decay=1e-4)
    loss_fn=nn.CrossEntropyLoss();tx=torch.tensor(x_train);ty=torch.tensor(y_train,dtype=torch.long)
    for epoch in range(180):
        order=torch.randperm(len(tx))
        for i in range(0,len(tx),64):
            batch=order[i:i+64];optimizer.zero_grad();loss=loss_fn(net(tx[batch]),ty[batch]);loss.backward();optimizer.step()
        if epoch in (60,120):
            for group in optimizer.param_groups:group["lr"]*=.35
    net.eval()
    qnet=Net();qnet.load_state_dict(net.state_dict())
    with torch.no_grad():
        for source,target in [(net.c1,qnet.c1),(net.c2,qnet.c2),(net.fc,qnet.fc)]:
            quant,scale=quantize(source.weight);target.weight.copy_(torch.tensor(quant.astype(np.float32)*scale))
        prediction=qnet(torch.tensor(x_test)).argmax(1).numpy()
    accuracy=accuracy_score(y_test,prediction)
    if accuracy<.85:raise SystemExit(f"held-out accuracy {accuracy:.3f} is below bootstrap gate")
    emit_header(args.out,net,mean,1/std,accuracy)
    print(f"wrote {args.out}; held-out quantized accuracy={accuracy:.4f}")

if __name__=="__main__":
    main()
