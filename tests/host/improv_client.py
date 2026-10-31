"""Python port of improv-wifi/sdk-serial-js packet logic (writePacketToStream + read loop + _handleIncomingPacket)."""
import subprocess, sys
HDR=[ord(c) for c in "IMPROV"]+[1]
def packet(typ, data):
    p=HDR+[typ,len(data)]+list(data)+[0,0]
    p[-2]=sum(p)&0xff; p[-1]=10
    return bytes(p)
def rpc(cmd, data=b""): return packet(0x03,[cmd,len(data)]+list(data))
def parse(stream):
    out=[]; line=[]; isImprov=None; L=0; text=[]
    for byte in stream:
        if isImprov:
            line.append(byte)
            if len(line)==L:
                payload=line[6:]; ver,typ,ln=payload[0],payload[1],payload[2]; data=payload[3:3+ln]
                ok=(sum(line[:-1])&0xff)==payload[3+ln]
                rec={"type":{1:"STATE",2:"ERROR",4:"RESULT"}.get(typ,typ),"ok_checksum":ok}
                if typ in (1,2): rec["value"]=data[0]
                if typ==4:
                    res=[]; total=data[1]; i=2
                    while i<2+total: res.append(bytes(data[i+1:i+1+data[i]]).decode()); i+=data[i]+1
                    rec["cmd"]=data[0]; rec["strings"]=res
                out.append(rec); isImprov=None; line=[]
            continue
        if byte==10:
            if line: text.append(bytes(line).decode(errors="replace"))
            line=[]; continue
        line.append(byte)
        if len(line)!=9: continue
        isImprov = bytes(line[:6])==b"IMPROV"
        if not isImprov: continue
        L=9+line[8]+1
    return out, text
def run(inp):
    p=subprocess.run([sys.argv[1] if len(sys.argv)>1 else "./improv_test"],input=inp,capture_output=True)
    return parse(p.stdout), p.stderr.decode().strip()
def wifi(ssid,pw):
    s=ssid.encode(); w=pw.encode(); return rpc(0x01, bytes([len(s)])+s+bytes([len(w)])+w)

print("1 current state (fresh):", run(rpc(0x02)))
print("2 device info:", run(rpc(0x03)))
print("3 scan:", run(rpc(0x04)))
print("4 provision good:", run(wifi("home-wifi","goodpass")))
print("5 provision bad:", run(wifi("home-wifi","nope")))
print("6 bad checksum:", run(bytes(bytearray(rpc(0x02))[:-2])+b"\x00\n"))
print("7 text cmd still works:", run(b"info\n"))
print("8 text then improv in one stream:", run(b"tilt off\n"+rpc(0x03)))
print("9 unknown rpc:", run(rpc(0x09)))
print("10 'IMP' text that isn't improv:", run(b"IMPish\n"))
