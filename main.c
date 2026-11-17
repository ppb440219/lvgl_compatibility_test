#include "lvgl/lvgl.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

typedef struct {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t mode_blob_id;
    drmModeModeInfo mode;

    uint32_t width;
    uint32_t height;
    uint32_t stride;     // 對齊後的每行 byte 數 (600 對齊為 608)
    uint32_t y_size;     // stride * height (608 * 1024 = 622592)
    uint32_t uv_size;    // stride * (height / 2)
    uint32_t total_size; // y_size + uv_size
    uint32_t handle;
    uint32_t fb_id;
    uint8_t * map;       // 指向 NV12 連續記憶體 (map = Y, map + y_size = UV)
} drm_nv12_ctx_t;

static drm_nv12_ctx_t g_drm = {0};
static uint8_t * g_lvgl_rgb_buf = NULL; // LVGL 繪製用的 XRGB8888 緩衝區
static lv_obj_t * vol_label;

static uint32_t custom_tick_get(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char * name)
{
    drmModeObjectProperties * props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;

    uint32_t prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes * prop = drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            if (strcmp(prop->name, name) == 0) {
                prop_id = prop->prop_id;
            }
            drmModeFreeProperty(prop);
        }
        if (prop_id) break;
    }
    drmModeFreeObjectProperties(props);
    return prop_id;
}

/* 高速整數運算：將 LVGL 的 XRGB8888 畫面轉換為硬體所需的 NV12 (Y + UV 雙平面) */
static void convert_xrgb8888_to_nv12(const uint8_t * src_xrgb, uint8_t * dst_nv12, uint32_t width, uint32_t height, uint32_t dst_stride)
{
    uint8_t * dst_y = dst_nv12;
    uint8_t * dst_uv = dst_nv12 + (dst_stride * height);
    const uint32_t * src32 = (const uint32_t *)src_xrgb;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint32_t * row0 = src32 + y * width;
        const uint32_t * row1 = src32 + (y + 1) * width;
        uint8_t * y_out0 = dst_y + y * dst_stride;
        uint8_t * y_out1 = dst_y + (y + 1) * dst_stride;
        uint8_t * uv_out = dst_uv + (y / 2) * dst_stride;

        for (uint32_t x = 0; x < width; x += 2) {
            uint32_t p00 = row0[x];
            uint32_t p01 = row0[x + 1];
            uint32_t p10 = row1[x];
            uint32_t p11 = row1[x + 1];

            int r00 = (p00 >> 16) & 0xFF, g00 = (p00 >> 8) & 0xFF, b00 = p00 & 0xFF;
            int r01 = (p01 >> 16) & 0xFF, g01 = (p01 >> 8) & 0xFF, b01 = p01 & 0xFF;
            int r10 = (p10 >> 16) & 0xFF, g10 = (p10 >> 8) & 0xFF, b10 = p10 & 0xFF;
            int r11 = (p11 >> 16) & 0xFF, g11 = (p11 >> 8) & 0xFF, b11 = p11 & 0xFF;

            y_out0[x]     = (uint8_t)(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            y_out0[x + 1] = (uint8_t)(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            y_out1[x]     = (uint8_t)(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            y_out1[x + 1] = (uint8_t)(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);

            int r_avg = (r00 + r01 + r10 + r11) >> 2;
            int g_avg = (g00 + g01 + g10 + g11) >> 2;
            int b_avg = (b00 + b01 + b10 + b11) >> 2;

            uv_out[x]     = (uint8_t)(((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128);
            uv_out[x + 1] = (uint8_t)(((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128);
        }
    }
}

/* 初始化 NV12 DRM Atomic 顯示管線 */
static int init_drm_nv12(const char * dev_path)
{
    g_drm.fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (g_drm.fd < 0) {
        perror("Failed to open DRM device");
        return -1;
    }

    drmSetClientCap(g_drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(g_drm.fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes * res = drmModeGetResources(g_drm.fd);
    if (!res) return -1;

    drmModeConnector * conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(g_drm.fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            g_drm.conn_id = conn->connector_id;
            g_drm.mode = conn->modes[0];
            g_drm.width = g_drm.mode.hdisplay;
            g_drm.height = g_drm.mode.vdisplay;
            break;
        }
        if (conn) drmModeFreeConnector(conn);
        conn = NULL;
    }
    if (!conn) {
        drmModeFreeResources(res);
        return -1;
    }

    uint32_t crtc_index = 0;
    if (conn->encoder_id) {
        drmModeEncoder * enc = drmModeGetEncoder(g_drm.fd, conn->encoder_id);
        if (enc) {
            g_drm.crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    if (!g_drm.crtc_id && res->count_crtcs > 0) g_drm.crtc_id = res->crtcs[0];
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == g_drm.crtc_id) {
            crtc_index = i;
            break;
        }
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    /* 尋找支援此 CRTC 且支援 DRM_FORMAT_NV12 的 Plane */
    drmModePlaneRes * plane_res = drmModeGetPlaneResources(g_drm.fd);
    if (!plane_res) return -1;

    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane * plane = drmModeGetPlane(g_drm.fd, plane_res->planes[i]);
        if (!plane) continue;

        if (plane->possible_crtcs & (1 << crtc_index)) {
            for (uint32_t f = 0; f < plane->count_formats; f++) {
                if (plane->formats[f] == DRM_FORMAT_NV12) {
                    g_drm.plane_id = plane->plane_id;
                    break;
                }
            }
        }
        drmModeFreePlane(plane);
        if (g_drm.plane_id) break;
    }
    drmModeFreePlaneResources(plane_res);

    if (!g_drm.plane_id) {
        printf("ERROR: No plane supporting NV12 found!\n");
        return -1;
    }

    printf("NV12 DRM Pipeline Locked: Connector=%u, CRTC=%u, NV12_Plane=%u (%ux%u)\n",
           g_drm.conn_id, g_drm.crtc_id, g_drm.plane_id, g_drm.width, g_drm.height);

    /* 關鍵修正：SigmaStar sstar_drm 要求寬度/stride 必須 16-byte 對齊 (600 -> 608) */
    uint32_t aligned_width = (g_drm.width + 15) & ~15;

    struct drm_mode_create_dumb creq = {
        .width = aligned_width,
        .height = g_drm.height * 3 / 2,
        .bpp = 8,
    };
    if (drmIoctl(g_drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB failed");
        return -1;
    }
    g_drm.stride = creq.pitch; // 608
    g_drm.y_size = g_drm.stride * g_drm.height; // 608 * 1024 = 622592
    g_drm.uv_size = g_drm.stride * (g_drm.height / 2);
    g_drm.total_size = creq.size;
    g_drm.handle = creq.handle;

    /* NV12 需要設定雙平面參數：Plane 0 = Y, Plane 1 = UV */
    uint32_t handles[4] = { g_drm.handle, g_drm.handle, 0, 0 };
    uint32_t pitches[4] = { g_drm.stride, g_drm.stride, 0, 0 };
    uint32_t offsets[4] = { 0, g_drm.y_size, 0, 0 };

    if (drmModeAddFB2(g_drm.fd, g_drm.width, g_drm.height, DRM_FORMAT_NV12,
                      handles, pitches, offsets, &g_drm.fb_id, 0) != 0) {
        perror("drmModeAddFB2 (NV12) failed");
        return -1;
    }

    struct drm_mode_map_dumb mreq = { .handle = g_drm.handle };
    if (drmIoctl(g_drm.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB failed");
        return -1;
    }

    g_drm.map = mmap(0, g_drm.total_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_drm.fd, mreq.offset);
    if (g_drm.map == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }

    /* 初始化為純黑畫面 (NV12 的純黑為 Y=16, UV=128) */
    memset(g_drm.map, 16, g_drm.y_size);
    memset(g_drm.map + g_drm.y_size, 128, g_drm.uv_size);

    /* 執行 Atomic Commit 綁定 NV12 Plane */
    drmModeCreatePropertyBlob(g_drm.fd, &g_drm.mode, sizeof(g_drm.mode), &g_drm.mode_blob_id);

    drmModeAtomicReq * req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, g_drm.conn_id, get_prop_id(g_drm.fd, g_drm.conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), g_drm.crtc_id);
    drmModeAtomicAddProperty(req, g_drm.crtc_id, get_prop_id(g_drm.fd, g_drm.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID"), g_drm.mode_blob_id);
    drmModeAtomicAddProperty(req, g_drm.crtc_id, get_prop_id(g_drm.fd, g_drm.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);

    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), g_drm.fb_id);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), g_drm.crtc_id);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X"), 0);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y"), 0);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W"), g_drm.width << 16);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H"), g_drm.height << 16);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"), 0);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"), 0);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"), g_drm.width);
    drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"), g_drm.height);

    int ret = drmModeAtomicCommit(g_drm.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);

    if (ret != 0) {
        perror("NV12 drmModeAtomicCommit failed");
        return -1;
    }
    printf("NV12 DRM Atomic Modeset SUCCESS! (stride=%u, y_size=%u)\n", g_drm.stride, g_drm.y_size);
    return 0;
}

/* LVGL 畫面刷新回呼：轉檔為 NV12 並透過 Atomic Commit 輸出 */
static void drm_nv12_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;

    if (lv_display_flush_is_last(disp)) {
        convert_xrgb8888_to_nv12(g_lvgl_rgb_buf, g_drm.map, g_drm.width, g_drm.height, g_drm.stride);
        drmModeAtomicReq * req = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(req, g_drm.plane_id, get_prop_id(g_drm.fd, g_drm.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), g_drm.fb_id);
        drmModeAtomicCommit(g_drm.fd, req, 0, NULL);
        drmModeAtomicFree(req);
    }
    lv_display_flush_ready(disp);
}

/* UI 互動事件 */
static void slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    lv_label_set_text_fmt(vol_label, "Volume: %d%%", (int)val);    
}

static void btn_event_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    if (lv_obj_has_state(btn, LV_STATE_CHECKED)) {
    lv_label_set_text(label, "MUTED");
    } else {
    lv_label_set_text(label, "AUDIO ON");
    }
}

int main(void)
{
    /* 1. 初始化 NV12 DRM Atomic 顯示管線 */
    if (init_drm_nv12("/dev/dri/card0") != 0) {
        return -1;
    }

    /* 2. 為 LVGL 配置獨立的 XRGB8888 繪圖緩衝區 */
    size_t rgb_buf_size = g_drm.width * g_drm.height * 4;
    g_lvgl_rgb_buf = malloc(rgb_buf_size);
    if (!g_lvgl_rgb_buf) {
        printf("Failed to allocate LVGL RGB buffer\n");
        return -1;
    }

    lv_init();
    lv_tick_set_cb(custom_tick_get);
    lv_display_t * disp = lv_display_create(g_drm.width, g_drm.height);
    lv_display_set_flush_cb(disp, drm_nv12_flush_cb);
    lv_display_set_buffers(disp, g_lvgl_rgb_buf, NULL, rgb_buf_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    /* 3. 初始化觸控螢幕 (/dev/input/event1 -> fts_ts) */
    lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
    /* 4. 建立測試介面 */
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1E1E2E), 0);
    vol_label = lv_label_create(scr);
    lv_label_set_text(vol_label, "Volume: 50%");
    lv_obj_set_style_text_color(vol_label, lv_color_hex(0xFFFFFF), 0);
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_24, 0);
#endif
    lv_obj_align(vol_label, LV_ALIGN_CENTER, 0, -100);
    lv_obj_t * slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 400, 30);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t * btn = lv_button_create(scr);
    lv_obj_set_size(btn, 220, 70);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "AUDIO ON");
#if LV_FONT_MONTSERRAT_24
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_24, 0);
#endif
    lv_obj_center(btn_label);

    /* 5. 進入主迴圈 */
    while (1) {
        uint32_t time_till_next = lv_timer_handler();
        usleep(time_till_next * 1000);
    }

    return 0;
}