import re,sys,collections
c=collections.Counter();t=0
for l in open(sys.argv[1],errors='replace'):
    if l.startswith('QEMU_GUEST_PROF:'):
        t+=int(re.search(r'PROF: (\d+)',l).group(1))
        for a,n in re.findall(r'0x([0-9a-f]+)=(\d+)',l):
            if a.startswith(sys.argv[2]): c[a]+=int(n)
for a,n in c.most_common(12): print(a,'%.1f%%'%(100*n/t))
