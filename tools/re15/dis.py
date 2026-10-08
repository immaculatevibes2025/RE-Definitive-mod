import struct
R=['zero','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7','s0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
class Img:
    def __init__(s,segs): s.segs=segs  # list of (base,bytes)
    def w(s,a):
        for b,d in s.segs:
            if b<=a<b+len(d)-3: return struct.unpack_from('<I',d,a-b)[0]
        return None
    def h(s,a):
        for b,d in s.segs:
            if b<=a<b+len(d)-1: return struct.unpack_from('<h',d,a-b)[0]
def dis(w,pc):
    op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; rd=(w>>11)&31; sh=(w>>6)&31; fn=w&63
    imm=w&0xffff; simm=imm-0x10000 if imm&0x8000 else imm
    if w==0: return 'nop'
    if op==0:
        n={0:'sll',2:'srl',3:'sra',4:'sllv',6:'srlv',7:'srav',8:'jr',9:'jalr',12:'syscall',13:'break',16:'mfhi',17:'mthi',18:'mflo',19:'mtlo',24:'mult',25:'multu',26:'div',27:'divu',32:'add',33:'addu',34:'sub',35:'subu',36:'and',37:'or',38:'xor',39:'nor',42:'slt',43:'sltu'}.get(fn,'?%d'%fn)
        if fn in(0,2,3): return '%s %s,%s,%d'%(n,R[rd],R[rt],sh)
        if fn==8: return 'jr %s'%R[rs]
        if fn==9: return 'jalr %s,%s'%(R[rd],R[rs])
        if fn in(16,18): return '%s %s'%(n,R[rd])
        if fn in(24,25,26,27): return '%s %s,%s'%(n,R[rs],R[rt])
        if fn in (4,6,7): return '%s %s,%s,%s'%(n,R[rd],R[rt],R[rs])
        return '%s %s,%s,%s'%(n,R[rd],R[rs],R[rt])
    if op==1:
        n={0:'bltz',1:'bgez',16:'bltzal',17:'bgezal'}.get(rt,'regimm')
        return '%s %s,0x%08x'%(n,R[rs],pc+4+simm*4)
    if op in(2,3): return '%s 0x%08x'%('j' if op==2 else 'jal',(pc&0xf0000000)|((w&0x3ffffff)<<2))
    if op in(4,5): return '%s %s,%s,0x%08x'%('beq' if op==4 else 'bne',R[rs],R[rt],pc+4+simm*4)
    if op in(6,7): return '%s %s,0x%08x'%('blez' if op==6 else 'bgtz',R[rs],pc+4+simm*4)
    names={8:'addi',9:'addiu',10:'slti',11:'sltiu',12:'andi',13:'ori',14:'xori',15:'lui',32:'lb',33:'lh',34:'lwl',35:'lw',36:'lbu',37:'lhu',38:'lwr',40:'sb',41:'sh',42:'swl',43:'sw',46:'swr',50:'lwc2',58:'swc2'}
    n=names.get(op)
    if op==18: return 'cop2 0x%07x'%(w&0x1ffffff) if rs&16 else 'cop2 %s %s,%d'%({0:'mfc2',2:'cfc2',4:'mtc2',6:'ctc2'}.get(rs,'?'),R[rt],rd)
    if op==16: return 'cop0 %d %s,%d'%(rs,R[rt],rd)
    if n is None: return '.word 0x%08x'%w
    if op==15: return 'lui %s,0x%04x'%(R[rt],imm)
    if op in(12,13,14): return '%s %s,%s,0x%x'%(n,R[rt],R[rs],imm)
    if op>=32: return '%s %s,%d(%s)'%(n,R[rt] if op not in(50,58) else '$%d'%rt,simm,R[rs])
    return '%s %s,%s,%d'%(n,R[rt],R[rs],simm)
def func(img,a,maxn=4000):
    out=[];pc=a;n=0;seen_jr=0
    while n<maxn:
        w=img.w(pc)
        if w is None: break
        out.append((pc,w,dis(w,pc))); n+=1
        if seen_jr: break
        if w==0x03e00008: seen_jr=1
        pc+=4
    return out
