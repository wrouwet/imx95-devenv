# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of m7shm.ko. On the board (kernel headers are in the image):
#   make            -> m7shm.ko and tools/m7shm-tool
#   make install    -> /lib/modules/$(uname -r)/updates/m7shm.ko, /usr/bin/m7shm-tool
obj-m := m7shm.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules tool

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

tool: tools/m7shm-tool

tools/m7shm-tool: tools/m7shm-tool.c m7shm_uapi.h
	$(CC) -O2 -Wall -Wextra -I$(CURDIR) -o $@ $<

install: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) INSTALL_MOD_DIR=updates modules_install
	depmod -a
	install -D -m 0755 tools/m7shm-tool $(DESTDIR)/usr/bin/m7shm-tool

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f tools/m7shm-tool

.PHONY: all modules tool install clean
