#!/usr/bin/env python3
"""Create an analytic renderer test scene; never infers or replaces game artwork.

The synthetic sphere has known normals and alternating metal/dielectric patches.
An adjacent identical unmapped sprite is a control for legacy fallback. Output
is a temporary PK3 loaded alongside a legal Doom IWAD for engine smoke tests.
"""
import argparse, io, math, struct, zipfile
from pathlib import Path
from PIL import Image, PngImagePlugin


def png(image, offsets=None):
    f=io.BytesIO(); metadata=PngImagePlugin.PngInfo()
    if offsets: metadata.add(b'grAb', struct.pack('>ii', *offsets))
    image.save(f,format='PNG',pnginfo=metadata); return f.getvalue()


def wad(lumps):
    data=bytearray(b'PWAD'+struct.pack('<II',len(lumps),0)); directory=[]
    for name,blob in lumps:
        directory.append((len(data),len(blob),name)); data.extend(blob)
    struct.pack_into('<I',data,8,len(data))
    for offset,size,name in directory:
        data.extend(struct.pack('<II8s',offset,size,name.encode().ljust(8,b'\0')))
    return bytes(data)


def create(destination):
    destination.mkdir(parents=True,exist_ok=True)
    n=96
    maps={name:Image.new('RGBA',(n,n),default) for name,default in {
        'color':(0,0,0,0),'normalmaps':(128,128,255,255),
        'metallic':(0,0,0,255),'roughness':(210,210,210,255),'ao':(255,255,255,255)}.items()}
    for y in range(n):
        for x in range(n):
            nx=(x+.5-n/2)/(n*.44);ny=(n/2-y-.5)/(n*.44);r=nx*nx+ny*ny
            if r>=1:continue
            z=math.sqrt(1-r)
            maps['color'].putpixel((x,y),(165,160,150,255))
            maps['normalmaps'].putpixel((x,y),tuple(round(128+127*v) for v in (nx,ny,z))+(255,))
            metal=255 if x<n//2 else 0
            rough=45 if y<n//2 else 175
            maps['metallic'].putpixel((x,y),(metal,metal,metal,255))
            maps['roughness'].putpixel((x,y),(rough,rough,rough,255))
    text=['namespace = "ZDoom";']
    verts=[(-256,-256),(-256,256),(256,256),(256,-256)]
    for x,y in verts:text.append(f'vertex {{ x={x}.0; y={y}.0; }}')
    text.append('sector { heightfloor=0; heightceiling=160; texturefloor="FLOOR0_1"; textureceiling="CEIL1_1"; lightlevel=112; }')
    for i in range(4):
        text.append('sidedef { sector=0; texturemiddle="STARTAN3"; }')
        text.append(f'linedef {{ v1={i}; v2={(i+1)%4}; sidefront={i}; blocking=true; }}')
    things=[(0,-180,0,1,90),(-80,44,0,15000,270),(80,44,0,15001,270),(-140,-24,98,15010,0),(140,20,96,15011,0)]
    for x,y,z,t,a in things:
        text.append(f'thing {{ x={x}.0; y={y}.0; height={z}.0; type={t}; angle={a}; skill1=true; skill2=true; skill3=true; skill4=true; skill5=true; single=true; coop=true; dm=true; }}')
    decorate='''actor SpritePBRTest 15000
{
  Radius 40
  Height 96
  +NOGRAVITY
  States { Spawn: PBRT A -1\n Stop }
}
actor SpriteLegacyControl 15001
{
  Radius 40
  Height 96
  +NOGRAVITY
  States { Spawn: PBRU A -1\n Stop }
}
actor SpriteTestKey 15010
{
  +NOGRAVITY
  +NOBLOCKMAP
  States { Spawn: TNT1 A -1\n Stop }
}
actor SpriteTestFill : SpriteTestKey 15011 {}
'''
    gldefs='''pointlight SpriteKey { color 1.0 0.72 0.40 size 220 attenuate 1 }
pointlight SpriteFill { color 0.2 0.45 1.0 size 160 attenuate 1 }
object SpriteTestKey { frame TNT1A { light SpriteKey } }
object SpriteTestFill { frame TNT1A { light SpriteFill } }
'''
    with zipfile.ZipFile(destination/'sprite-lighting-test.pk3','w',zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('DECORATE',decorate)
        archive.writestr('GLDEFS',gldefs)
        archive.writestr('MAPINFO','map PBRTEST "Sprite materials validation" { next = "PBRTEST" sky1 = "SKY1" }')
        archive.writestr('maps/PBRTEST.wad',wad([('PBRTEST',b''),('TEXTMAP','\n'.join(text).encode()),('ENDMAP',b'')]))
        archive.writestr('sprites/PBRTA0.png',png(maps['color'],(48,96)))
        archive.writestr('sprites/PBRUA0.png',png(maps['color'],(48,96)))
        for kind,image in maps.items():
            if kind!='color':archive.writestr(f'materials/{kind}/auto/PBRTA0.png',png(image))
    cfg=['wait 2','r_preset 3','r_preset_auto false','r_adaptive_quality false','r_taa false','r_upscale 1','r_grain 0','r_vignette 0','gl_bloom false','r_volumetric 0','r_ssr 0','r_ssgi 0','gl_ssao 0','r_gpuparticles 0','r_sprite_pbr true','r_sprite_reflections true','gl_light_sprites true','screenblocks 12','crosshair 0','show_messages false','map PBRTEST','wait 175']
    for name,cmd in [('lit','r_sprite_debug 0'),('normals','r_sprite_debug 1'),('roughness','r_sprite_debug 2'),('metallic','r_sprite_debug 3'),('legacy','r_sprite_pbr false')]:
        cfg += [cmd,'wait 10',f'screenshot "{destination/name}.png"']
    cfg+=['r_sprite_pbr true','r_sprite_debug 0']
    # GZDoom's wait defers the rest of one command string, not later exec lines.
    (destination/'capture.cfg').write_text('; '.join(cfg)+'\n')
    print(destination/'sprite-lighting-test.pk3')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('destination',type=Path);create(p.parse_args().destination.resolve())
