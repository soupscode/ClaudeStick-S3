import sys
from PIL import Image
out=sys.argv[1]; names=sys.argv[2:]
ims=[Image.open(n+'.ppm').resize((480,270),Image.NEAREST) for n in names]
rows=(len(ims)+1)//2
W=Image.new('RGB',(990,280*rows+10),(40,40,40))
for i,im in enumerate(ims): W.paste(im,(10+(i%2)*490,10+(i//2)*280))
W.save(out)
