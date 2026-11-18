# AudioBox UI demo

兩個平台共用 720x1280 直式 LVGL 畫布，不旋轉螢幕或觸控座標：

- `PLATFORM=sstar`（預設）：原有 Yocto / SigmaStar NV12 輸出。
- `PLATFORM=rpi`：Raspberry Pi DRM/KMS XRGB8888 輸出，不做 NV12 轉換。

實體模式仍使用第一個已連線螢幕的第一個 DRM mode；必須至少為
720x1280。800x1280 螢幕的右側 80 像素保持黑色。
Touch Display 2 5 吋原生為 720x1280，因此不需要旋轉。

## 初始化 LVGL

兩個平台都使用專案內的 `lvgl/` submodule。首次 clone 後請執行：

```sh
git submodule update --init --recursive
```

## Yocto 平台

```sh
. ~/projects/yocto_toolchain/environment-setup-aarch64-poky-linux
make PLATFORM=sstar -j$(nproc)
```

## Raspberry Pi 5 本機編譯

在 Raspberry Pi OS 上安裝依賴，不要載入 Yocto SDK 環境：

```sh
sudo apt install build-essential libdrm-dev
make PLATFORM=rpi -j$(nproc)
```

LVGL 設定沿用專案內的 `lv_conf.h`（32-bit 色彩、evdev）。
兩個平台的物件檔分別放在 `build/sstar`、`build/rpi`；
每次 make 都重新連結共用的 `audiobox_gui`，避免切換平台時使用錯誤產物。

## 執行

直接 DRM/KMS 模式需要控制顯示裝置；請停用圖形桌面，從文字終端執行。
本機編譯仍需要 `libdrm-dev`，只是會使用 Raspberry Pi 自己的標頭和函式庫。

```sh
sudo ./audiobox_gui /dev/dri/card1 /dev/input/event0
```

以上裝置路徑只是範例。請先用 `modetest -M vc4 -c -p` 檢查顯示端，
用 `cat /proc/bus/input/devices` 檢查觸控裝置；`modetest` 可由
`sudo apt install libdrm-tests` 安裝。Pi 5 的顯示卡編號不一定是 `card0`，
請使用有已連線 DSI connector 的 KMS 裝置，不要使用 `renderD*`。
若同時接 HDMI，本 demo 會選第一個已連線 connector；測試時建議只接 DSI 螢幕。

不帶參數時沿用原機器預設的 `/dev/dri/card0`、`/dev/input/event1`。
觸控處理仍沿用 LVGL evdev，沒有加入旋轉、軸交換或校正設定。
