#===============================================================================
# export variable
#===============================================================================
ifeq ($(CFG_HI_EXPORT_FLAG),)
SDK_DIR := $(shell cd $(CURDIR)/../.. && /bin/pwd)
include $(SDK_DIR)/base.mak
endif

SRC_DIR := $(CURDIR)

CFLAGS := -Wall -D_FILE_OFFSET_BITS=64
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
          -I$(SRC_DIR)

SYS_LIBS := -lc -lpthread -lrt -lstdc++ -ldl
HI_LIBS := -ljpeg -lhi_common -lpng -lhigo -lhigoadp -lhi_msp -lhi_resampler -lz -lfreetype -lhi_subtitle -lhi_so -lhi_ttx -lhi_cc

HI_DEPEND_LIBS := -L$(SRC_DIR)/lib/$(CFG_HI_ARM_TOOLCHAINS_NAME) -L$(HI_SHARED_LIB_DIR)
HI_DEPEND_LIBS += $(SYS_LIBS) $(HI_LIBS) -lfuse3

#===============================================================================
# local variable
#===============================================================================
OUT := AVServer

LOCAL_SRCS := AVServer.c \
              player.c \
			  string_ext.c

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

$(OUT): clean
	@$(CFG_HI_ARM_TOOLCHAINS_NAME)-gcc -Wall -D_FILE_OFFSET_BITS=64 $(CFLAGS) $(HI_DEPEND_LIBS) -o $(OUT) $(LOCAL_SRCS) $(COMMON_SRCS)

clean:
	@rm -f $(OUT)