# Applies core/supla-device-multidevice.patch to the supla-device copy in
# .pio/libdeps. Runs as a post script: libraries are installed by the main
# build script, but sources are compiled only after all scripts are read.

import os
import subprocess

Import("env")

patch = os.path.join(env.subst("$PROJECT_DIR"), "core",
                     "supla-device-multidevice.patch")
lib_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"),
                       env.subst("$PIOENV"), "SuplaDevice")


def git_apply(*args):
    return subprocess.run(["git", "apply"] + list(args) + [patch],
                          cwd=lib_dir, capture_output=True, text=True)


if not os.path.isdir(lib_dir):
    print("Z2S: %s not found, supla-device patch not applied" % lib_dir)
    env.Exit(1)

if git_apply("--reverse", "--check").returncode != 0:
    result = git_apply()
    if result.returncode != 0:
        print("Z2S: cannot apply %s:\n%s" % (patch, result.stderr))
        env.Exit(1)
    print("Z2S: applied %s" % os.path.basename(patch))
