from img import *
def xrefs(I,segs,target):
    hi=(target+0x8000)>>16; lo=target-(hi<<16)
    res=[]
    for base,d in segs:
        for o in range(0,len(d)-4,4):
            w=struct.unpack_from('<I',d,o)[0]
            if w>>26==15 and (w&0xffff)==hi:
                r=(w>>16)&31
                for k in range(1,12):
                    if o+4*k>=len(d)-4: break
                    w2=struct.unpack_from('<I',d,o+4*k)[0]
                    if ((w2>>21)&31)==r and (w2&0xffff)==(lo&0xffff) and w2>>26 in(9,35,33,37,36,32,43):
                        res.append(base+o+4*k); break
    return res
