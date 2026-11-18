1. for project use yocto toolchain
   1. launch toolchain manually
   2. make PLATFORM=sstar
   3. copy 'audiobox_gui' binary to device and execute

2. for rpi 5 (build on rpi5)
   1. fetch code:
      - git clone git@github.com:ppb440219/lvgl_compatibility_test.git
      - cd lvgl_compatibility_test & git submodule update --init --recursive
   1. sudo apt install build-essential git libdrm-dev libevdev-dev
   1. clone project into rpi5
   3. make PLATFORM=rpi
   4. execution: 'sudo ./audiobox_gui /dev/dri/card1 /dev/input/event0'
