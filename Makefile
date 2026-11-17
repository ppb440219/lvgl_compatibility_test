# 執行檔名稱
TARGET = audiobox_gui

# 編譯器設定：直接使用 Yocto SDK 匯出的 $(CC)，若未載入環境則預設 gcc
CC ?= gcc

# 專案與 LVGL 路徑
# LVGL 原始碼為共享目錄，可用 `make LVGL_DIR=/path/to/lvgl` 覆寫
LVGL_DIR ?= $(HOME)/projects/lvgl
BUILD_DIR = build

# 編譯參數：
#   -I.                      尋找 lv_conf.h
#   -I$(dir $(LVGL_DIR))     讓 main.c 的 #include "lvgl/lvgl.h" 可以找到共享的 lvgl 目錄
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

# 自動遞迴尋找共享 lvgl/src 下的所有 .c 檔案，
# 其 .o 另外對應到 build/lvgl/ 底下，避免寫入共享的 LVGL 目錄
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -name "*.c")
LVGL_OBJS = $(patsubst $(LVGL_DIR)/%.c, $(BUILD_DIR)/lvgl/%.o, $(LVGL_SRCS))

OBJS = $(COBJS) $(LVGL_OBJS)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
