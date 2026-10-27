obj-m := cbuf.o

KDIR ?= $(shell if [ -d /lib/modules/$$(uname -r)/build ]; then echo /lib/modules/$$(uname -r)/build; else ls -d /lib/modules/*/build 2>/dev/null | sort -V | tail -n 1; fi)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
