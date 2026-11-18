# 執行檔名稱
TARGET = audiobox_gui

# 編譯器設定：直接使用 Yocto SDK 匯出的 $(CC)，若未載入環境則預設 gcc
CC ?= gcc

# sstar: Yocto SDK / NV12; rpi: native Raspberry Pi / XRGB8888
PLATFORM ?= sstar
ifeq ($(PLATFORM),sstar)
    CPPFLAGS += -DUSE_NV12=1
else ifeq ($(PLATFORM),rpi)
    CPPFLAGS += -DUSE_NV12=0
else
    $(error Unsupported PLATFORM '$(PLATFORM)'; use sstar or rpi)
endif

# 專案與 LVGL 路徑
LVGL_DIR := $(CURDIR)/lvgl
BUILD_DIR = build/$(PLATFORM)

ifeq ($(wildcard $(LVGL_DIR)/lvgl.h),)
ifneq ($(MAKECMDGOALS),clean)
    $(error LVGL submodule is missing; run 'git submodule update --init --recursive')
endif
else
    LVGL_SRCS := $(shell find "$(LVGL_DIR)/src" -name "*.c")
endif

# 編譯參數：
#   -I.                      尋找 lv_conf.h
#   -I$(dir $(LVGL_DIR))     讓 main.c 的 #include "lvgl/lvgl.h" 可以找到 LVGL
#   -I$(LVGL_DIR)            LVGL 內部標頭
INC = -I. -I$(patsubst %/,%,$(dir $(LVGL_DIR))) -I$(LVGL_DIR)
ifneq ($(OECORE_TARGET_SYSROOT),)
    INC += -I$(OECORE_TARGET_SYSROOT)/usr/include/libdrm
else
    INC += -I/usr/include/libdrm
endif

CFLAGS += -O2 -Wall -Wextra $(INC) -DLV_CONF_INCLUDE_SIMPLE
LDFLAGS += -ldrm -lm -lpthread

# 專案本身的原始碼
CSRCS = main.c
COBJS = $(patsubst %.c, $(BUILD_DIR)/%.o, $(CSRCS))

LVGL_OBJS = $(patsubst $(LVGL_DIR)/%.c, $(BUILD_DIR)/lvgl/%.o, $(LVGL_SRCS))

OBJS = $(COBJS) $(LVGL_OBJS)

.PHONY: all clean $(TARGET)

all: $(TARGET)

# Makefile 變更時重編，避免沿用切換 LVGL 路徑前的物件檔
$(OBJS): Makefile

$(TARGET): $(OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
