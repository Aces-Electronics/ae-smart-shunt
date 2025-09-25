# firmware/version.py
Import("env")
import os

ver = os.environ.get("OTA_VERSION", "dev")
# Make it a proper C/C++ string literal: -DOTA_VERSION="v0.0.1"
env.Append(CPPDEFINES=[("OTA_VERSION", env.StringifyMacro(ver))])
print(f"## PIO: OTA_VERSION set to {ver}")