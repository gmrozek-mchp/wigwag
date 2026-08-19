#!/usr/bin/env bash
#
# Fetch and verify the RNWF02 module firmware package and Microchip's DFU utilities.
#
# The binaries are not committed: they are vendor-licensed, several megabytes, and would go stale.
# What is committed is this script -- the URLs and the SHA-256 of every artefact we have actually
# flashed a module with, so a future session gets byte-identical inputs or a loud failure.
#
#   firmware/tools/fetch-rnwf02-firmware.sh            # download, verify, unpack
#   firmware/tools/fetch-rnwf02-firmware.sh --verify    # verify an existing cache, download nothing
#
# Everything lands in firmware/tools/vendor/ (gitignored). See docs/module-firmware-dfu.md.
#
# CHECKING FOR SOMETHING NEWER: the version below is not "latest" forever, it is what was latest on
# 2026-08-18. Releases are published on the SDK's GitHub releases page,
# https://github.com/MicrochipTech/WINCS02-RNWF02-SDK/releases -- which is *not* where 3.1.0 came from
# (that was the RNWF02 Firmware software-library page, and it still lists 3.1.0 as newest). Check the
# SDK, not the product page.
#
# Bumping is not a one-line change: 3.2.0 renamed every image (*.bootable.bin -> *_wholeflash.bin,
# *_dfu.bootable.bin -> *_dfu_high.bin), so a new release means the URL, the filenames and all the
# checksums together, in one commit, with the change recorded in JOURNAL.md. Per ADR-0025 the policy
# is the latest released firmware, not any particular version.
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

VERSION="3.2.0"

# The revision hash is part of the asset filename, so it changes with every release.
FW_REV="a1ac4a49"
FW_URL="https://github.com/MicrochipTech/WINCS02-RNWF02-SDK/releases/download/v${VERSION}/RNWF02_Module_Release_${VERSION}_${FW_REV}.zip"
FW_SHA="20da9a2dec20e1901a078f4581a516be42ddb57ad91edb701450069dd3431b86"

# The DFU utility is versioned separately from the firmware (this zip contains pyDFU 2.0.1).
UTILS_URL="https://ww1.microchip.com/downloads/aemDocuments/documents/WSG/ProductDocuments/SoftwareTools/rnwf-wilc-winc-utilities.zip"
UTILS_SHA="71d64bc817a1ba8eedaecc59b18db431109928f32e5c20961732090b31ba12b4"

# Images inside the firmware zip, checked after unpacking. rnwf02_ota.bin is the one we normally
# flash -- it is what nvm-update.py writes to a *running* module over the AT UART (D141) -- and
# rnwf02_dfu_high.bin is the DFU recovery image for a module that will not boot. The others are
# recorded so a substituted package is caught rather than silently used.
#
# flfs_image.bin is byte-identical to 3.1.0's: the certificate store did not change between releases.
IMAGE_SHAS="
516491e5d91e13a2aa4c5a08c104b88462e8f7bf7f3866bccb72a0d547c4209b  bin/rnwf02_ota.bin
c396ef8087118468713ca5c57ba2bfca16148e997fa25f0904386b743870e538  bin/rnwf02_dfu_high.bin
34d8a2d54737463bb81981d97026935366f17fa1a8e70eb9b66bb90d2e739eea  bin/rnwf02_wholeflash.bin
6693d8acdae03b826f3493ae4eecd62ab2f13d60fd57b2f3f4ed9d170a5150b9  bin/flfs_image.bin
"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
vendor="${here}/vendor"
verify_only=0
[ "${1:-}" = "--verify" ] && verify_only=1

sha_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

check() {
	local path="$1" want="$2" name="$3" got
	got="$(sha_of "$path")"
	if [ "$got" != "$want" ]; then
		echo "FAIL ${name}: sha256 mismatch" >&2
		echo "  want ${want}" >&2
		echo "  got  ${got}" >&2
		echo "  Microchip may have re-cut the archive under the same name. Do not flash this."	>&2
		echo "  Diff the release notes, then update the checksum in this script deliberately."	>&2
		return 1
	fi
	echo "ok   ${name}"
}

fetch() {
	local url="$1" dest="$2" want="$3" name="$4"

	if [ -f "$dest" ] && [ "$(sha_of "$dest")" = "$want" ]; then
		echo "ok   ${name} (cached)"
		return 0
	fi

	if [ "$verify_only" = 1 ]; then
		echo "FAIL ${name}: missing or wrong, and --verify was given" >&2
		return 1
	fi

	echo "-->  ${name}"
	curl -fsSL --retry 3 -o "$dest" "$url"
	check "$dest" "$want" "$name"
}

mkdir -p "$vendor"

fetch "$FW_URL"    "${vendor}/RNWF02-Firmware-${VERSION}.zip" "$FW_SHA"    "firmware ${VERSION} package"
fetch "$UTILS_URL" "${vendor}/rnwf-wilc-winc-utilities.zip"   "$UTILS_SHA" "DFU utilities"

if [ "$verify_only" = 0 ]; then
	rm -rf "${vendor}/fw" "${vendor}/utils"
	mkdir -p "${vendor}/fw" "${vendor}/utils"
	unzip -q -o "${vendor}/RNWF02-Firmware-${VERSION}.zip" -d "${vendor}/fw"
	unzip -q -o "${vendor}/rnwf-wilc-winc-utilities.zip"   -d "${vendor}/utils"
fi

# 3.1.0 unpacked to RN_release_<version>/, 3.2.0 to RNWF02_module_release_<version>/. Match both, so
# this keeps working if the layout changes again.
release="$(find "${vendor}/fw" -maxdepth 1 -type d \( -name 'RNWF02_module_release_*' -o -name 'RN_release_*' \) | head -1)"
dfu_dir="$(find "${vendor}/utils" -type d -name dfu | head -1)"

if [ -z "$release" ] || [ -z "$dfu_dir" ]; then
	echo "FAIL: unpacked layout not as expected under ${vendor}" >&2
	exit 1
fi

echo "$IMAGE_SHAS" | while read -r want path; do
	[ -z "${want:-}" ] && continue
	check "${release}/${path}" "$want" "$(basename "$path")"
done

cat <<EOF

Ready. Paths for the DFU driver:

  --utils-dir ${dfu_dir}                                  (DFU recovery: module will not boot)
  image       ${release}/bin/rnwf02_dfu_high.bin

Normal updates go over the AT UART instead, with the SDK's own tool (D141):

  nvm-update.py -p /dev/cu.usbmodemXXXX ${release}/bin/rnwf02_ota.bin

Next: docs/module-firmware-dfu.md
EOF
