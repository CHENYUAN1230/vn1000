# Comment/uncomment the following line to disable/enable debugging
#DEBUG = y


# Add your debugging flag (or not) to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DVNIC_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

ccflags-y += $(DEBFLAGS)

ifneq ($(KERNELRELEASE),)
# call from kernel build system
ccflags-y += -I$(src)/../include

# Build-specific half: VNIC_BACKEND=engine (in-kernel kthread, default) or =pci
# (real PCI/MMIO/DMA/MSI). vnic-core.o is shared; the linker resolves the
# vnic_hal_* calls against whichever half is linked in.
VNIC_BACKEND ?= engine
obj-m	:= vnic.o
vnic-y	:= vnic-core.o vnic-$(VNIC_BACKEND).o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif



clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.mod modules.order *.symvers

depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M *.c > .depend


ifeq (.depend,$(wildcard .depend))
include .depend
endif
