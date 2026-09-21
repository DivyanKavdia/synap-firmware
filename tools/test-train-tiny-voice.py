#!/usr/bin/env python3
"""Offline trainer checks: python3 tools/test-train-tiny-voice.py."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import numpy as np
from scipy.io import wavfile

spec=importlib.util.spec_from_file_location('trainer',Path(__file__).with_name('train-tiny-voice.py'))
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)

class TrainingTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        wavfile.write(self.root/'one.wav',16000,np.arange(32000,dtype=np.int16))
    def tearDown(self):self.temp.cleanup()
    def manifest(self,rows):
        path=self.root/'manifest.json';path.write_text(json.dumps(rows));return path
    def row(self,**changes):
        row=dict(path='one.wav',label='audio',start=0,end=.8,session='first',split='train');row.update(changes);return row
    def test_same_session_rejected(self):
        with self.assertRaisesRegex(ValueError,'session'):
            t.load_real_manifest(self.manifest([self.row(),self.row(start=1,end=1.8,split='test')]))
    def test_renamed_recording_cannot_leak(self):
        shutil.copyfile(self.root/'one.wav',self.root/'copy.wav')
        with self.assertRaisesRegex(ValueError,'same recording'):
            t.load_real_manifest(self.manifest([self.row(),self.row(path='copy.wav',start=1,end=1.8,session='pretend-new',split='test')]))
    def test_invalid_interval_rejected(self):
        for end in [-1,0,4,float('nan')]:
            with self.assertRaises(ValueError):t.load_real_manifest(self.manifest([self.row(end=end)]))
    def test_independent_sessions_and_deterministic_holdout(self):
        wavfile.write(self.root/'two.wav',16000,np.arange(32000,dtype=np.int16)[::-1])
        rows=t.load_real_manifest(self.manifest([self.row(),self.row(path='two.wav',session='second',split='test')]))
        x,y=t.real_features(rows,'test');xx,yy=t.real_features(rows,'test')
        self.assertEqual(x.shape,(1,30,12));np.testing.assert_array_equal(x,xx)
        self.assertEqual(y.tolist(),[6]);np.testing.assert_array_equal(y,yy)
        tx,ty=t.real_features(rows,'train',3);self.assertEqual(len(ty),3)
    def test_exported_header_compiles_for_eight_class_runtime(self):
        header=self.root/'model.h'
        t.emit_header(header,t.Net(),np.zeros(12),np.ones(12),.9)
        source=self.root/'check.cpp'
        source.write_text('#include "model.h"\nusing namespace ChakshuTinyModel;\n'
          'static_assert(CLASSES==8 && AUDIO==6 && DESCRIBE==7);\n'
          'static_assert(sizeof(FC_WEIGHT)==CLASSES*CHANNELS*2);\n'
          'static_assert(LEARNED_WEIGHT_BYTES==2240);\nint main(){}\n')
        subprocess.run(['g++','-std=c++17','-fsyntax-only',str(source)],check=True)

if __name__=='__main__':unittest.main()
