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
# 2026-08-18. The release packages are published on the RNWF02 Add-on Board product page
# (https://www.microchip.com/en-us/development-tool/ev72e72a) and on the RNWF02 Firmware software
# library page. If a newer one exists, bump VERSION and the checksum together, in one commit, and
# record the change in JOURNAL.md -- per ADR-0025 the policy is to ship the latest released firmware,
# not to stay on 3.1.0 forever.
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

VERSION="3.1.0"

FW_URL="https://ww1.microchip.com/downloads/aemDocuments/documents/WSG/ProductDocuments/SoftwareLibraries/Firmware/RNWF02-Firmware-and-Release-Notes-${VERSION}.zip"
FW_SHA="4b1c41bb58983ec25768f89f590e156b45c899a3085225fab75761cbf231326e"

# The DFU utility is versioned separately from the firmware (this zip contains pyDFU 2.0.1).
UTILS_URL="https://ww1.microchip.com/downloads/aemDocuments/documents/WSG/ProductDocuments/SoftwareTools/rnwf-wilc-winc-utilities.zip"
UTILS_SHA="71d64bc817a1ba8eedaecc59b18db431109928f32e5c20961732090b31ba12b4"

# Images inside the firmware zip, checked after unpacking. rnwf02_dfu.bootable.bin is the one we
# flash; the others are recorded so a substituted package is caught rather than silently used.
IMAGE_SHAS="
98230a146b52968bfb00ac710a508207bd8b73b9c0d6840fd44c36cd9f155f15  bin/rnwf02_dfu.bootable.bin
4027906e9fc967914abe38a1fc0f228b15199ad0c8be53fa41bf079369691a82  bin/rnwf02.bootable.bin
5e465e9bc73bbb49aeefee958aa4bdad621ed49893ea38504a0a2ba0280cafcf  bin/rnwf02_ota.bin
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

release="$(find "${vendor}/fw" -maxdepth 1 -type d -name 'RN_release_*' | head -1)"
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

  --utils-dir ${dfu_dir}
  image       ${release}/bin/rnwf02_dfu.bootable.bin

Next: docs/module-firmware-dfu.md
EOF
