#!/usr/bin/env bash
#
# Provision the on-device LLVM/Swift toolchain that CoreCompiler needs.
#
# Prefer a PREBUILT toolchain (a Release asset) — downloading it skips the
# multi-hour, network-fragile from-source build entirely. If the prebuilt isn't
# published yet, build from source with a RESILIENT fetch: the Swift toolchain
# sources are huge and the runner's network is flaky, so update-checkout is
# retried (it resumes, skipping already-cloned repos). The first successful build
# is captured + published by CI, so every later build just downloads it.
set -euo pipefail

SWIFT_BRANCH="swift-6.3.2-RELEASE"
PREBUILT_TAG="prebuilt-llvm-on-ios"
PREBUILT_URL="https://github.com/sr-hawk/LemexDE/releases/download/${PREBUILT_TAG}/prebuilt-toolchain.tar.zst"
OUT="CoreCompiler/CoreCompilerSupportLibs"

# 1) Try the prebuilt.
if curl -fsSL -o /tmp/ccsl.tar.zst "$PREBUILT_URL"; then
    echo "[toolchain] using prebuilt toolchain ($PREBUILT_TAG)"
    rm -rf "$OUT"
    mkdir -p CoreCompiler
    zstd -dc /tmp/ccsl.tar.zst | tar -x -C CoreCompiler
    exit 0
fi

# 2) Build from source, resilient fetch.
echo "[toolchain] prebuilt not found — building LLVM-On-iOS from source"
cd LLVM-On-iOS

fetched=0
for i in $(seq 1 10); do
    if SWIFT_BRANCH="$SWIFT_BRANCH" SWIFT_SOURCE_DIR="swift" Scripts/build-swift-toolchain.sh fetch; then
        fetched=1
        break
    fi
    echo "[toolchain] fetch attempt $i failed; retrying in 30s"
    sleep 30
done
[ "$fetched" = 1 ] || { echo "[toolchain] fetch failed after retries"; exit 1; }

# The lld/MachO patch the LLVM-On-iOS 'swift:' target applies after fetch
# (idempotent: skip if already patched).
if ! grep -q "NYXIAN: apple lies" llvm-project/lld/MachO/InputFiles.cpp; then
    perl -i -0pe 's|(// Swift LLVM fork downstream change start\n)(.*?)(// Swift LLVM fork downstream change end\n)|$1/* NYXIAN: apple lies, lld works fine for MachO\n$2*/\n$3|s' \
        llvm-project/lld/MachO/InputFiles.cpp
fi

# Sources are present + patched, so 'make' skips its own fetch and just builds.
make

cd ..
rm -rf "$OUT"
cp -r LLVM-On-iOS/CoreCompilerSupportLibs "$OUT"
cp -r LLVM-On-iOS/LLVM.xcframework "$OUT/LLVM.xcframework"
echo "[toolchain] built from source"
