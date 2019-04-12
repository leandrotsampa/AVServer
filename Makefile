#===============================================================================
# export variable
#===============================================================================
ifeq ($(CFG_HI_EXPORT_FLAG),)
SDK_DIR := $(shell cd $(CURDIR)/../.. && /bin/pwd)
include $(SDK_DIR)/base.mak
endif

SRC_DIR := $(CURDIR)

CFLAGS := -Wall -D_GNU_SOURCE -D_XOPEN_SOURCE=600 -D_FILE_OFFSET_BITS=64
CFLAGS += -DCHIP_TYPE_$(CFG_HI_CHIP_TYPE) -DCFG_HI_SDK_VERSION=$(CFG_HI_SDK_VERSION)
CFLAGS += $(CFG_HI_BOARD_CONFIGS)

ifeq ($(CFG_HI_HDMI_SUPPORT_HDCP), y)
CFLAGS += -DHI_HDCP_SUPPORT
endif

ifeq ($(CFG_HI_HDMI_RX_SUPPORT), y)
CFLAGS += -DHI_HDMI_RX_INSIDE
endif

CFLAGS += -I$(HI_INCLUDE_DIR) \
          -I$(MSP_DIR)/include \
          -I$(MSP_DIR)/api/include \
          -I$(MSP_DIR)/drv/include \
          -I$(SAMPLE_DIR)/common \
          -I$(SRC_DIR)/include \
          -I$(SRC_DIR)/libfuse \
          -I$(SRC_DIR)

SYS_LIBS := -lc -lpthread -lrt -lstdc++ -ldl
HI_LIBS := -ljpeg -lhi_common -lpng -lhigo -lhigoadp -lhi_msp -lhi_resampler -lz -lfreetype -lhi_subtitle -lhi_so -lhi_ttx -lhi_cc

HI_DEPEND_LIBS := -L$(HI_SHARED_LIB_DIR) $(SYS_LIBS) $(HI_LIBS)

#===============================================================================
# local variable
#===============================================================================
OUT     := AVServer
VERSION := \"$(shell date "+%Y.%m-%d (Build: %H%M%S)")\"

LOCAL_SRCS := AVServer.c \
              player.c \
			  string_ext.c

CFLAGS += -DFUSERMOUNT_DIR=\"/tmp\" -D_REENTRANT -DFUSE_USE_VERSION=31
LIBFUSE_SRCS := libfuse/fuse.c \
                libfuse/fuse_i.h \
                libfuse/fuse_loop.c \
                libfuse/fuse_loop_mt.c \
                libfuse/fuse_lowlevel.c \
                libfuse/fuse_misc.h \
                libfuse/fuse_opt.c \
                libfuse/fuse_signals.c \
                libfuse/buffer.c \
                libfuse/cuse_lowlevel.c \
                libfuse/helper.c \
                libfuse/mount.c \
                libfuse/mount_util.c \
                libfuse/mount_util.h \
                libfuse/modules/subdir.c \
                libfuse/modules/iconv.c

COMMON_SRCS := $(SAMPLE_DIR)/common/hi_adp_demux.c \
               $(SAMPLE_DIR)/common/hi_adp_data.c \
               $(SAMPLE_DIR)/common/hi_adp_hdmi.c \
               $(SAMPLE_DIR)/common/hi_adp_mpi.c \
               $(SAMPLE_DIR)/common/hi_adp_search.c \
               $(SAMPLE_DIR)/common/hi_filter.c

ifeq ($(CFG_HI_FRONTEND_SUPPORT),y)
COMMON_SRCS += $(SAMPLE_DIR)/common/hi_adp_tuner.c
endif

ifeq ($(CFG_HI_PVR_SUPPORT),y)
COMMON_SRCS += $(SAMPLE_DIR)/common/hi_adp_pvr.c
endif

#===============================================================================
# rules
#===============================================================================
default: $(OUT)

strip: $(OUT)
	@$(CFG_HI_ARM_TOOLCHAINS_NAME)-strip --strip-all $(OUT)

$(OUT): clean version
	@$(CFG_HI_ARM_TOOLCHAINS_NAME)-gcc -Wall -D_FILE_OFFSET_BITS=64 $(CFLAGS) $(HI_DEPEND_LIBS) -o $(OUT) $(LOCAL_SRCS) $(LIBFUSE_SRCS) $(COMMON_SRCS)

clean:
	@rm -f $(OUT)

version:
	@echo "#define AVSERVER_VERSION $(VERSION)">version.h