import re,glob,json,sys,pickle,collections
G='/mnt/user-data/uploads/Quake4/rex/q4/generated/default/'
FN={}
def load():
  for f in glob.glob(G+'q4_recomp.*.cpp'):
    t=open(f,errors='ignore').read()
    for m in re.finditer(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]+)\) \{\n(.*?)(?=^DEFINE_REX_FUNC|\Z)',t,re.S|re.M):
      a=int(m.group(1),16); ins=[]; addr=a; labels=set()
      for l in m.group(2).split('\n'):
        lm=re.match(r'^loc_([0-9A-F]+):',l)
        if lm: labels.add(int(lm.group(1),16)); continue
        mm=re.match(r'\s*// (\S+)\s*(.*)$',l)
        if mm: ins.append((addr,mm.group(1),mm.group(2))); addr+=4
      FN[a]=(ins,labels)
load()
NODEST=re.compile(r'^(st|cmp|b|mt|tw|dcb|sync|eieio|isync|lwsync|nop|icbi|trap)')
def analyze(a,lo,hi):
  ins,labels=FN[a]; regs={}; uses=[]
  for (ad,mn,ops) in ins:
    if ad in labels: regs={}   # conservative: block boundary
    o=[x.strip() for x in ops.split(',')] if ops else []
    if mn=='lis':
      regs[o[0]]=(int(o[1],0)<<16)&0xffffffff; continue
    # uses
    for m in re.finditer(r'(-?\d+)\((r\d+)\)',ops):
      r=m.group(2)
      if r in regs:
        ea=(regs[r]+int(m.group(1)))&0xffffffff; uses.append((ad,mn,ops,r,ea))
    if mn in('addi','la') and len(o)==3 and o[1] in regs:
      ea=(regs[o[1]]+int(o[2],0))&0xffffffff; uses.append((ad,mn,ops,o[1],ea))
    if mn.startswith('bl'): 
      for r in ['r0']+['r%d'%i for i in range(3,13)]: regs.pop(r,None)
    if o and not NODEST.match(mn) and o[0].startswith('r'):
      regs.pop(o[0],None)
    if mn in ('lwzu','lbzu','lhzu','stwu','stbu','sthu','lfsu','lfdu','stfsu','stfdu'):
      m=re.search(r'\((r\d+)\)',ops)
      if m: regs.pop(m.group(1),None)
  return uses

LO,HI=0x82FE7928,0x8300FA00
def sites(a):
  ins,labels=FN[a]; out=[]
  for i,(ad,mn,ops) in enumerate(ins):
    if mn!='lis': continue
    o=[x.strip() for x in ops.split(',')]; r=o[0]; v=(int(o[1],0)<<16)&0xffffffff
    if not (0x82FE0000<=v<=0x83010000): continue
    uses=[]
    for (ad2,mn2,ops2) in ins[i+1:]:
      o2=[x.strip() for x in ops2.split(',')] if ops2 else []
      for m in re.finditer(r'(-?\d+)\(('+r+r')\)',ops2):
        uses.append((ad2,mn2,(v+int(m.group(1)))&0xffffffff))
      if mn2 in('addi',) and len(o2)==3 and o2[1]==r:
        uses.append((ad2,mn2,(v+int(o2[2],0))&0xffffffff))
      elif r in o2[1:] and not re.search(r'\('+r+r'\)',ops2):
        uses.append((ad2,mn2,'REG'))
      if (o2 and o2[0]==r and not NODEST.match(mn2)) : break
      if mn2.startswith('bl') and r in ['r0']+['r%d'%k for k in range(3,13)]: break
    inr=[u for u in uses if u[2]!='REG' and LO<=u[2]<HI]
    oth=[u for u in uses if not(u[2]!='REG' and LO<=u[2]<HI)]
    if inr: out.append((ad,r,v,inr,oth))
  return out
fs=[0x8229E560,0x8229E770,0x8229EA80,0x8229EDA8,0x8229EEF8,0x8229F410,0x8229F7F0,0x8229FA78,0x822A0508,0x822A7440,0x822A6D90,0x822A70F8]
entries=[]; regs=set()
for a in fs:
  for (ad,r,v,inr,oth) in sites(a):
    assert not oth, (hex(ad),oth)
    entries.append((ad,r)); regs.add(r)
print(len(entries), sorted(regs))
out=['',
'# Front-end helper threads (rb_thread.cpp, "front-end helpers"): the shadow volume code keeps its',
'# working state (shadow index buffer, counters, scratch pointers) in static variables at',
'# 0x82FE7928-0x8300FA00. After each `lis` that forms the base address of that block in those',
'# functions, a helper thread rebases the register onto its own copy of the block, so several',
'# threads can build shadow volumes at once. On any other thread the delta is 0. Sites found by',
'# tools/gen_fe_hooks.py (every use of each base register stays inside the block).']
for ad,r in sorted(entries):
  out+=['','[[midasm_hook]]','address = 0x%08X'%ad,'name = "Q4Fe_Rebase_%s"'%r,'registers = ["%s"]'%r,'after_instruction = true']
open('/home/claude/q4hle/q4_rb_hooks.toml','a').write('\n'.join(out)+'\n')
c=['','// ---- front-end helper threads: per-thread shadow volume state (q4_rb_hooks.toml) ----',
'namespace q4fe {','// byte offset from the shadow code\'s static block (0x82FE7928) to this thread\'s copy; 0 = shared','thread_local int32_t t_delta = 0;','}  // namespace q4fe','']
for r in sorted(regs, key=lambda x:int(x[1:])):
  c.append('void Q4Fe_Rebase_%s(PPCRegister& %s) { if (q4fe::t_delta) %s.s64 = int32_t(%s.u32 + uint32_t(q4fe::t_delta)); }'%(r,r,r,r))
open('/home/claude/q4hle/src/q4_tagscope.cpp','a').write('\n'.join(c)+'\n')
