import struct, sys
img=open(r'C:\Quake4\work\image.bin','rb').read()
BASE=0x82000000
lo,hi=0x82150000-BASE,0x82963604-BASE
n=(hi-lo)//4
w=struct.unpack('>%dI'%n, img[lo:lo+n*4])
def addr(i): return BASE+lo+i*4
def run(i, f, minlen):
    k=0
    while i+k<n and f(w[i+k],k): k+=1
    return k
res={}
# gpr: std r14..r31 at -0x98.. ; ld likewise
def std_chain(op):  # op 0xF8000000 std, 0xE8000000 ld
    return lambda x,k: x==(op|((14+k)<<21)|(1<<16)|((-0x98+8*k)&0xFFFC))
def fp_chain(op):
    return lambda x,k: x==(op|((14+k)<<21)|(1<<16)|((-0x90+8*k)&0xFFFF))
for name,f in [('savegprlr_14',std_chain(0xF8000000)),('restgprlr_14',std_chain(0xE8000000)),
               ('savefpr_14',fp_chain(0xD8000000)),('restfpr_14',fp_chain(0xC8000000))]:
    for i in range(n):
        if run(i,f,18)>=18:
            print(name, hex(addr(i)), 'tail:', [hex(w[i+18+j]) for j in range(3)])
# vmx: alternating li r12,-X ; stvx/lvx
def li12(x): return (x>>16)==0x3980
for i in range(n-4):
    if li12(w[i]) and (w[i+1]>>26) in (31,4):
        k=0
        while i+2*k+1<n and li12(w[i+2*k]) and (w[i+2*k+1]>>26) in (31,4): k+=1
        if k>=16 and not (i>=2 and li12(w[i-2]) and (w[i-1]>>26) in (31,4)):
            op=w[i+1]
            print('vmx run at',hex(addr(i)),'len',k,'first li',hex(w[i]&0xFFFF),'insn',hex(op),'last',hex(w[i+2*k-1]))
