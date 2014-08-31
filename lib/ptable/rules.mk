LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_DEPS += \
	lib/bio \
	lib/cksum

MODULE_SRCS += \
	$(LOCAL_DIR)/ptable.c

include make/module.mk
