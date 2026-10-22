"""Reuse the exact C6 pattern verifier for Xorg/GLX PRIME scanout."""
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('c7_prime_capture', Path(__file__).with_name('nouveau-prime-capture.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class FrameCapture(capture.FrameCapture):
    def __call__(self, log):
        super().__call__(log.replace('NOUVEAU_C7_XORG: READY', 'NOUVEAU_C6_PRESENT: READY'))

    def finish(self):
        if len(self.frames) != self.frame_count:
            raise RuntimeError('C7 Xorg scanout frames missing')
        result = dict(status='pass', width=capture.WIDTH, height=capture.HEIGHT,
                      frames=self.frames, scope='NVIDIA NVK/Zink GLX -> Xorg modesetting/glamor -> native VirtIO PRIME scanout; separate exact shader FBO checks')
        (self.directory/'capture.json').write_text(json.dumps(result, indent=2) + '\n')
