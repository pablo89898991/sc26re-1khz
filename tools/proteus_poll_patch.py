import struct,zlib,sys
src=sys.argv[1] if len(sys.argv)>1 else 'PROTEUS_FW_6AC6869B.fw'
out=sys.argv[2] if len(sys.argv)>2 else '.'
f=bytearray(open(src,'rb').read()); hdr=f[:32]; pl=bytearray(f[32:])
assert len(pl)==struct.unpack_from('<I',hdr,4)[0] and zlib.crc32(pl)==struct.unpack_from('<I',hdr,8)[0]
WAITS=[0xad64,0xadd0,0xae2a,0xaea0]
FS=[0x31bb8,0x31bc6,0x31bfd,0x31c0b,0x31c42,0x31c50,0x31c87,0x31c95]
HS=[0x31bbf,0x31bcd,0x31c04,0x31c12,0x31c49,0x31c57,0x31c8e,0x31c9c]
# Keyboard filter: the Puck forwards a keyboard report (0x41) only when it holds
# nothing but Enter, Esc, Tab and the arrows with no modifier (the controller's
# lizard keys) and drops every other one. Branch from the size check straight
# to the send, so every keyboard report from the controller reaches the PC.
KBD=0xa484  # adds r0, r6, #4 -> b.n 0xa4aa (send)
def kbd_unlock(p):
    o=KBD-0x8000; assert p[o:o+2]==bytes.fromhex('301d') or p[o:o+2]==bytes.fromhex('11e0'),'kbd filter not found'
    p[o:o+2]=bytes.fromhex('11e0')
def mk(ticks, fs, hs, name):
    p=bytearray(pl)
    kbd_unlock(p)
    for a in WAITS:
        o=a-0x8000; assert p[o:o+2]==bytes([0x63,0x22]),hex(a); p[o]=ticks
    for a in FS:
        o=a-0x8000; assert p[o:o+6]==bytes.fromhex('0705')+p[o+2:o+3]+bytes.fromhex('034000') and p[o+6]==2,hex(a); p[o+6]=fs
    for a in HS:
        o=a-0x8000; assert p[o+6]==5,hex(a); p[o+6]=hs
    h=bytearray(hdr); struct.pack_into('<II',h,4,len(p),zlib.crc32(p)&0xffffffff)
    open(name,'wb').write(h+p); print(name, hex(zlib.crc32(p)))
if True:
    p=bytearray(pl); kbd_unlock(p)
    h=bytearray(hdr); struct.pack_into('<II',h,4,len(p),zlib.crc32(p)&0xffffffff)
    n=f'{out}/PROTEUS_FW_6AC6869B_kbd.fw'; open(n,'wb').write(h+p); print(n, hex(zlib.crc32(p)))
for t,fs,hs,n in ((66,2,5,'poll2ms_kbd'),(33,1,4,'poll1ms_kbd'),(16,1,4,'poll05ms_kbd'),(1,1,4,'pollmax_kbd')):
    mk(t,fs,hs,f'{out}/PROTEUS_FW_6AC6869B_{n}.fw')
