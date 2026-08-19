# =============================================================================
#  disable_sbr.py  (PlatformIO pre-build script)
# -----------------------------------------------------------------------------
#  libhelix の ConfigHelix.h が定義する HELIX_FEATURE_AUDIO_CODEC_AAC_SBR を
#  ビルド前に無効化する。これで HE-AAC の SBR デコード(50KB超のメモリ)を使わず、
#  AAC-LC ベースのみを復号する（音は少しこもるが、省メモリで途切れにくい）。
#
#  ※ ライブラリ本体を手編集せず、毎ビルド時に自動で(冪等に)パッチするので、
#     .platformio を消してライブラリを取り直しても再適用される。
#  ※ SBR を復活させたい場合は platformio.ini の extra_scripts 行を外すか、
#     ~/.platformio/... の ConfigHelix.h を元に戻す。
# =============================================================================
Import("env")
import os
import glob

MARKER_DONE = "RADIKO_SBR_DISABLED"
TARGET_LINE = "#  define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR"

def patch(path):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            src = f.read()
    except OSError:
        return
    if MARKER_DONE in src:
        return  # already patched
    if TARGET_LINE not in src:
        return
    src = src.replace(
        TARGET_LINE,
        "// " + MARKER_DONE + " : SBR disabled to save RAM (radiko build)\n//" + TARGET_LINE,
    )
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    print("[disable_sbr] patched:", path)

libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
if not libdeps or not os.path.isdir(libdeps):
    libdeps = os.path.join(env.subst("$PROJECT_DIR"), ".pio", "libdeps")

for p in glob.glob(os.path.join(libdeps, "**", "ConfigHelix.h"), recursive=True):
    patch(p)
