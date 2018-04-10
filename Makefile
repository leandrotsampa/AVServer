#===============================================================================
# export variable
#===============================================================================
ifeq ($(CFG_HI_EXPORT_FLAG),)
SDK_DIR := $(shell cd $(CURDIR)/../.. && /bin/pwd)
include $(SDK_DIR)/base.mak
endif

include $(SAMPLE_DIR)/base.mak

#===============================================================================
# local variable
#===============================================================================
AVSERVER_DIR := $(shell pwd)

CFLAGS += -D_GNU_SOURCE -D_XOPEN_SOURCE=600
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

CFLAGS += -I$(HI_INCLUDE_DIR) \
          -I$(MSP_DIR)/include \
          -I$(MSP_DIR)/api/include \
          -I$(MSP_DIR)/drv/include \
          -I$(SAMPLE_DIR)/common \
          -I$(AVSERVER_DIR)/include \
          -I$(AVSERVER_DIR)

SAMPLE_IMAGES := AVServer test_av

LOCAL_OBJS := player.o string_ext.o $(COMMON_SRCS:%.c=%.o)

DEPEND_LIBS := $(HI_LIBS)
DEPEND_LIBS += -L$(AVSERVER_DIR)/lib/$(CFG_HI_ARM_TOOLCHAINS_NAME) -lfuse3

include $(SAMPLE_DIR)/hi_sample_rules.mak
