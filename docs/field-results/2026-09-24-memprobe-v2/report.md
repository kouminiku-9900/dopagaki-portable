# memprobe report

## memprobe-m1.txt (variant m1)

- devkit 0x06060110, kuKernelGetModel 8
- installed komi-tube EBOOT MEMSIZE: 2 (-1 = key absent)
- free `boot`: total 49.56 MB, largest 49.32 MB
- free `after-avcodec`: total 49.56 MB, largest 49.32 MB
- free `after-mpeg-vsh`: total 49.52 MB, largest 49.27 MB
- free `after-net-modules`: total 49.10 MB, largest 48.85 MB
- free `after-net-init`: total 48.93 MB, largest 48.73 MB
- free `after-layout`: total 3.43 MB, largest 2.23 MB
- free map `boot`: 2 ranges, below 0x0A000000 22.82 MB, above 26.75 MB
  - 0x0892EE00-0x0BA80000 (49.32 MB)
  - 0x0BAC0000-0x0BAFF000 (0.25 MB)
- free map `after-modules`: 2 ranges, below 0x0A000000 22.23 MB, above 26.71 MB
  - 0x089C5B00-0x0BA80000 (48.73 MB)
  - 0x0BAC0000-0x0BAF5000 (0.21 MB)
- free map `after-layout`: 3 ranges, below 0x0A000000 3.23 MB, above 0.21 MB
  - 0x089C5B00-0x08C00000 (2.23 MB)
  - 0x09200000-0x09300000 (1.00 MB)
  - 0x0BAC0000-0x0BAF5000 (0.21 MB)
- ME region: 0x08C00000-0x09200000 (6.00 MB, aligned, below the limit)
- arena: 0x09300000-0x0BA80000 (39.50 MB, 26.50 MB above the limit)
- volatile memory lock 0x00000000 at 0x08400000 size 4194304
- Media Engine reads (AAC decode):
  - me-region: ok (24/24)
  - volatile: ok (24/24)
  - mixed-io-high: ok (24/24)
  - arena-high: ok (24/24)
- 10 codec cycles: 10/10, partition change 0 bytes
- module / net init statuses:
  - `module name=avcodec status=0x00000000`
  - `module name=mpeg_vsh load=0x03EC6477 start=0x03EC6477 module_status=0x00000000`
  - `module name=net-common status=0x00000000`
  - `module name=net-inet status=0x00000000`
  - `net stage=sceNetInit status=0x00000000`
  - `net stage=sceNetInetInit status=0x00000000`
  - `net stage=sceNetResolverInit status=0x00000000`
  - `net stage=sceNetApctlInit status=0x00000000`

