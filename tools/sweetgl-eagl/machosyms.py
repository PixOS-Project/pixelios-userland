import sys,struct,glob
out=set()
for a in sys.argv[1:]:
    d=open(a,'rb').read(); assert d[:8]==b'!<arch>\n'; p=8
    while p<len(d):
        h=d[p:p+60]; name=h[:16].decode().strip(); size=int(h[48:58]); p+=60; body=d[p:p+size]; p+=size+(size&1)
        if name.startswith('#1/'):
            n=int(name[3:]); body=body[n:]
        if body[:4]!=bytes.fromhex('cffaedfe'): continue
        ncmds=struct.unpack_from('<I',body,16)[0]; o=32
        for _ in range(ncmds):
            cmd,sz=struct.unpack_from('<II',body,o)
            if cmd==2:
                symoff,nsyms,stroff,strsize=struct.unpack_from('<IIII',body,o+8)
                for i in range(nsyms):
                    strx,t,sect,desc,val=struct.unpack_from('<IBBHQ',body,symoff+16*i)
                    if (t&0x0e)==0x0e and (t&1) and sect!=0:
                        e=body.index(b'\0',stroff+strx); out.add(body[stroff+strx:e].decode())
            o+=sz
print('\n'.join(sorted(out)))
