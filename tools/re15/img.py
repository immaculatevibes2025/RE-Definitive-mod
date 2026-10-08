import sys; sys.path.insert(0,'/tmp/claude-0/mips')
from dis import *
exe=open('/mnt/user-data/uploads/Biohazard 2 [1.5][Original Unaltered]/PSX.EXE','rb').read()[0x800:]
def image(stage=None):
    segs=[(0x80010000,exe)]
    if stage: segs.append((0x80100000,open('/mnt/user-data/uploads/PSX/BIN/STAGE%d.BIN'%stage,'rb').read()))
    return Img(segs)
