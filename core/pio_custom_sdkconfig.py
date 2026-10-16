# Helpers for the custom_sdkconfig build (framework = arduino), which first
# rebuilds the IDF libraries with a dummy sketch, copies them into the core
# package and then builds Z2S in a second run.

import shutil
from pathlib import Path

Import("env")

custom_sdkconfig = env.GetProjectOption("custom_sdkconfig", "")
lib_compile = env.subst("$ARDUINO_LIB_COMPILE_FLAG") == "Build"

if lib_compile:
    # The rebuilt IDF components expect their embedded text files as
    # $BUILD_DIR/<name>.S, but the platform generates those only for
    # framework = espidf.
    for f in env.GetProjectOption("custom_embed_asm_txtfiles", "").splitlines():
        if f.strip():
            env.FileToAsm(str(Path("$PROJECT_DIR") / f.strip()), FILE_TYPE="TEXT")

    # The dummy sketch is linked with Z2S_Library, which needs symbols
    # defined in the real sketch. That firmware is thrown away.
    env.Append(LINKFLAGS=["-Wl,--unresolved-symbols=ignore-all"])

    # The platform copies only esp-idf/<component>/*.a; the mbedTLS
    # libraries are one level deeper. Copy them before it starts the
    # second run (its copy step is a post action of checkprogsize).
    nested = {
        "mbedtls/mbedtls/library/libmbedtls.a": "libmbedtls_2.a",
        "mbedtls/mbedtls/library/libmbedx509.a": "libmbedx509.a",
        "mbedtls/mbedtls/library/libmbedcrypto.a": "libmbedcrypto.a",
        "mbedtls/mbedtls/3rdparty/everest/libeverest.a": "libeverest.a",
        "mbedtls/mbedtls/3rdparty/p256-m/libp256m.a": "libp256m.a",
    }

    def copy_nested_libs(source, target, env):
        src_dir = Path(env.subst("$BUILD_DIR")) / "esp-idf"
        dst_dir = (Path(env.PioPlatform().get_package_dir(
            "framework-arduinoespressif32-libs"))
            / env.BoardConfig().get("build.mcu") / "lib")
        for src, dst in nested.items():
            shutil.copyfile(src_dir / src, dst_dir / dst)
            print("Z2S: copied %s" % dst)

    env.AddPreAction("checkprogsize", copy_nested_libs)

elif custom_sdkconfig:
    # The core links with --wrap=log_printf (log hook of esp_diagnostics),
    # but the wrapper is compiled only with CONFIG_LIB_BUILDER_COMPILE,
    # which exists only in Espressif's lib-builder. Call log_printf directly.
    env.Replace(LINKFLAGS=[f for f in env.get("LINKFLAGS", [])
                           if f != "-Wl,--wrap=log_printf"])

    # Linker flags of the core are not regenerated; dynamic buffers work
    # through these wraps (components/mbedtls/CMakeLists.txt).
    if "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y" in custom_sdkconfig:
        env.Append(LINKFLAGS=["-Wl,--wrap=%s" % f for f in (
            "mbedtls_ssl_write_client_hello",
            "mbedtls_ssl_handshake_client_step",
            "mbedtls_ssl_tls13_handshake_client_step",
            "mbedtls_ssl_handshake_server_step",
            "mbedtls_ssl_read",
            "mbedtls_ssl_write",
            "mbedtls_ssl_session_reset",
            "mbedtls_ssl_free",
            "mbedtls_ssl_setup",
            "mbedtls_ssl_send_alert_message",
            "mbedtls_ssl_close_notify",
        )])
