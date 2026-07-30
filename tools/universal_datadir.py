"""
PlatformIO pre-script for the esp32-universal env: serve the new universal web
UI (esp32/webui) from LittleFS instead of the legacy esp32/esp32_slide_whistle/
data folder (review item #3). `pio run -e esp32-universal -t uploadfs` then
flashes the universal interface.
"""
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

env.Replace(  # noqa: F821
    PROJECT_DATA_DIR=os.path.join(env["PROJECT_DIR"], "esp32", "webui")  # noqa: F821
)
print("[universal] LittleFS data dir -> esp32/webui")
