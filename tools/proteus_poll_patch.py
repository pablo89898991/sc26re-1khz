import struct,zlib,sys
src='/mnt/user-data/uploads/36c57bda-PROTEUS_FW_6AC6869B.fw'
f=bytearray(open(src,'rb').read()); hdr=f[:32]; pl=bytearray(f[32:])
assert len(pl)==struct.unpack_from('<I',hdr,4)[0] and zlib.crc32(pl)==struct.unpack_from('<I',hdr,8)[0]
WAITS=[0xad64,0xadd0,0xae2a,0xaea0]
FS=[0x31bb8,0x31bc6,0x31bfd,0x31c0b,0x31c42,0x31c50,0x31c87,0x31c95]
HS=[0x31bbf,0x31bcd,0x31c04,0x31c12,0x31c49,0x31c57,0x31c8e,0x31c9c]
def mk(ticks, fs, hs, name):
    p=bytearray(pl)
    for a in WAITS:
        o=a-0x8000; assert p[o:o+2]==bytes([0x63,0x22]),hex(a); p[o]=ticks
    for a in FS:
        o=a-0x8000; assert p[o:o+6]==bytes.fromhex('0705')+p[o+2:o+3]+bytes.fromhex('034000') and p[o+6]==2,hex(a); p[o+6]=fs
    for a in HS:
        o=a-0x8000; assert p[o+6]==5,hex(a); p[o+6]=hs
    h=bytearray(hdr); struct.pack_into('<II',h,4,len(p),zlib.crc32(p)&0xffffffff)
    open(name,'wb').write(h+p); print(name, hex(zlib.crc32(p)))
mk(66,2,5,'/home/claude/work/out/PROTEUS_FW_6AC6869B_poll2ms.fw')
mk(33,1,4,'/home/claude/work/out/PROTEUS_FW_6AC6869B_poll1ms.fw')
