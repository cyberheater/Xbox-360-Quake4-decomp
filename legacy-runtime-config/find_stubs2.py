import struct
img=open(r'C:\Quake4\work\image.bin','rb').read()
B=0x82000000; lo=0x82150000-B; hi=0x82963604-B; n=(hi-lo)//4
w=struct.unpack('>%dI'%n,img[lo:lo+n*4])
ad=lambda i:B+lo+i*4
# consecutive stfd/lfd rA=r1, increasing fpr numbers by 1, offsets by 8
for opn,op in (('stfd',54),('lfd',50)):
    i=0
    while i<n:
        x=w[i]
        if x>>26==op and (x>>16)&31==1:
            k=1
            while i+k<n and w[i+k]>>26==op and (w[i+k]>>16)&31==1 and ((w[i+k]>>21)&31)==((w[i+k-1]>>21)&31)+1: k+=1
            if k>=8: print(opn,hex(ad(i)),'count',k,'first f%d'%((x>>21)&31),'off',(x&0xFFFF)-(0x10000 if x&0x8000 else 0)); 
            i+=k
        else: i+=1
# vector: li r12 / lis / stvx pairs: look for any stvx(31/231) or lvx(31/103) with ra=12 or 11
import collections
hits=[]
for i in range(n):
    x=w[i]
    if x>>26==31:
        xo=(x>>1)&0x3FF
        if xo in (231,103): hits.append((i,xo))
runs=[];cur=[]
for i,xo in hits:
    if cur and i-cur[-1][0]<=2 and xo==cur[-1][1]: cur.append((i,xo))
    else:
        if len(cur)>=10: runs.append(cur)
        cur=[(i,xo)]
if len(cur)>=10: runs.append(cur)
for r in runs: print('vmx run',hex(ad(r[0][0])),'count',len(r),'xo',r[0][1])
