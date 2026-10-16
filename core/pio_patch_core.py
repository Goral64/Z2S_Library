# Copies core/HTTPUpdateServer.h over the one shipped with the Arduino core,
# like the GitHub workflow does for arduino-cli builds.

import filecmp
import os
import shutil

Import("env")

src = os.path.join(env.subst("$PROJECT_DIR"), "core", "HTTPUpdateServer.h")
dst = os.path.join(
    env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
    "libraries", "HTTPUpdateServer", "src", "HTTPUpdateServer.h")

if not filecmp.cmp(src, dst, shallow=False):
    shutil.copyfile(src, dst)
    print("Z2S: replaced %s" % dst)
