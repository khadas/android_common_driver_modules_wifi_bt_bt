# SPDX-License-Identifier: (GPL-2.0+ OR MIT)

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC)/$(M)/configs/5_15 M=$(M)/configs/5_15 KERNEL_SRC=$(KERNEL_SRC) $(@)
