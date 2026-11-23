# JOE weather (`fw/joe-weather` 62ee5b0), mocks, not for merging

`w-sheet.png` shows six faces from `classic_fw.py`, a port of `drawJoeClassic()` including its weather code:
- 19 °C, clear, light
- 8 °C, rain, dark, a FROM count
- -12 °C, snow
- a reading older than 3 hours (`--°F`), dark
- 102 °F, sun and cloud
- 16 °C, storm, dark

`a-sheet.png` is the same layout with no place set, which shows only the moon. `wxicons.png` shows the seven pictures (`wxicons.py`, in the order `WXI_SUN` to `WXI_STORM` in weather.h).
