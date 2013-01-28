#!makefile

KERNEL_DIR ?= /home/yuq/code/linux-a10

.PHONY : all clean
all:
	@$(MAKE) -C $(KERNEL_DIR) M=$$PWD modules

clean:
	@$(MAKE) -C $(KERNEL_DIR) M=$$PWD clean


