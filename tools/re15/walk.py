from img import *
I=image(5)
OV=(0x80100000,0x80100000+len(I.segs[1][1]))
def inov(a): return OV[0]<=a<OV[1]
def walk(entry):
    funcs={}; todo=[entry]; ext=set()
    while todo:
        f=todo.pop()
        if f in funcs: continue
        pcs=set(); work=[f]; body=[]
        while work:
            pc=work.pop()
            while pc not in pcs:
                w=I.w(pc)
                if w is None: break
                pcs.add(pc); op=w>>26
                s=dis(w,pc)
                if op==3:
                    t=(pc&0xf0000000)|((w&0x3ffffff)<<2)
                    (todo.append(t) if inov(t) else ext.add(t))
                if op in (4,5,6,7) or (op==1):
                    imm=w&0xffff; simm=imm-0x10000 if imm&0x8000 else imm
                    work.append(pc+4+simm*4)
                if op==2:
                    t=(pc&0xf0000000)|((w&0x3ffffff)<<2); pcs.add(pc+4); work.append(t); break
                if w==0x03e00008: pcs.add(pc+4); break
                if op==0 and (w&63)==8: pcs.add(pc+4); break
                pc+=4
        funcs[f]=sorted(pcs)
        # jump tables loaded via lui 0x8012 addiu -> table of overlay ptrs
        for pc in funcs[f]:
            w=I.w(pc)
            if w>>26==15:
                hi=(w&0xffff)<<16; r=(w>>16)&31
                w2=I.w(pc+4)
                if w2 and w2>>26==9 and ((w2>>21)&31)==r:
                    t=hi+(I.h(pc+4))
                    if inov(t):
                        # table of pointers
                        k=0
                        while True:
                            v=I.w(t+4*k)
                            if v is None or not inov(v): break
                            if I.w(v-4) is not None and v not in funcs: todo.append(v)
                            k+=1
    return funcs,ext
