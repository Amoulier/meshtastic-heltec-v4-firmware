#!/usr/bin/env python3
# trunk-ignore-all(ruff/F821)
# trunk-ignore-all(flake8/F821): For SConstruct imports
import os

from readprops import readProps

Import("env")
platform = env.PioPlatform()
verObj = readProps(env["PROJECT_DIR"] + "/version.properties")
env.Replace(MESHTASTIC_VERSION=verObj)
os.environ["SOURCE_DATE_EPOCH"] = str(verObj["build_epoch"])
env["ENV"]["SOURCE_DATE_EPOCH"] = str(verObj["build_epoch"])

if platform.name == "native":
    env.Replace(PROGNAME="meshtasticd")
else:
    env.Replace(PROGNAME=f"firmware-{env.get('PIOENV')}-{verObj['long']}")
    env.Replace(ESP32_FS_IMAGE_NAME=f"littlefs-{env.get('PIOENV')}-{verObj['long']}")

# Print the new program name for verification
print(f"PROGNAME: {env.get('PROGNAME')}")
if platform.name == "espressif32":
    # The hybrid IDF library caches esp_app_desc; its sdkconfig must bind this version too.
    project_config = env.GetProjectConfig()
    sdkconfig = env.GetProjectOption("custom_sdkconfig", "")
    sdkconfig += (
        "\nCONFIG_APP_PROJECT_VER_FROM_CONFIG=y\n"
        f'CONFIG_APP_PROJECT_VER="{verObj["long"]}"\n'
        "CONFIG_APP_REPRODUCIBLE_BUILD=y\n"
    )
    project_config.set("env:" + env["PIOENV"], "custom_sdkconfig", sdkconfig)
    print(f"ESP32_FS_IMAGE_NAME: {env.get('ESP32_FS_IMAGE_NAME')}")
