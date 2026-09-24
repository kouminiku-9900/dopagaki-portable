# Built once per MEMSIZE variant by scripts/build_memprobe.sh, which runs
# make from build/memprobe/<variant>/ with SRCDIR pointing here.
SRCDIR ?= .
VARIANT ?= m2
MEMSIZE ?= 2
vpath %.c $(SRCDIR)

TARGET = memprobe
OBJS = main.o
BUILD_PRX = 1
PSP_LARGE_MEMORY = $(MEMSIZE)
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -DMEMPROBE_VARIANT='"$(VARIANT)"'
# psputility, pspnet_inet and pspnet_resolver come from the toolchain specs,
# after libc; naming them here too splits their import stubs.
LIBS = -lpspaudiocodec -lpspkubridge -lpspnet_apctl -lpspnet
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = komi-tube memprobe ($(VARIANT))
PSP_EBOOT_ICON = $(SRCDIR)/../../assets/ICON0.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
