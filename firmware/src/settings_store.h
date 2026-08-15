/*
 * Persistence for wigwag's settings, in the storage partition the board devicetree already reserves.
 *
 * This is what ADR-0017's flash driver was built for. Until now the Wi-Fi passphrase and broker were
 * string literals in flash, costing no RAM and requiring a rebuild and a reflash to change a password
 * (D37/D56). Now they are set over the console and stored, and the build-time values are only
 * defaults for a device that has never been configured.
 *
 * NVS rather than a hand-rolled record, because the hard part is not storing bytes — it is staying
 * consistent across a power cut mid-write and wearing the partition's eight sectors evenly. A corrupt
 * settings store on a device whose only output is three lamps is a bad failure, and hand-rolled
 * crash-safe flash update is exactly where subtle bugs live.
 *
 * Note it takes the flash device explicitly. FIXED_PARTITION_DEVICE() cannot work on this family —
 * PL10 declares flash0 directly under /soc, so partition-to-device resolution finds /soc rather than a
 * device (ADR-0017, upstream bug 5). The offset still comes from devicetree.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include "settings.h"

/**
 * Fill @p s with build-time defaults, then overlay whatever is stored.
 *
 * Never fails in a way the caller must handle: if the store will not mount or is empty, the defaults
 * stand and the device runs exactly as it did before this file existed. Returns 0 if the store
 * mounted, or a negative errno if it did not — worth saying on the console, not worth refusing to boot
 * over, because a light that works with stale settings beats a light that does not work at all.
 */
int settings_load(struct wigwag_settings *s);

/** Persist every field. NVS skips writes whose value is unchanged, so calling this is cheap. */
int settings_save(const struct wigwag_settings *s);

/**
 * Forget everything stored and reset @p s to build-time defaults.
 *
 * The escape hatch for a unit configured onto a network that no longer exists, and what a long button
 * press should eventually do (D58).
 */
int settings_clear(struct wigwag_settings *s);

/** Build-time defaults only, no flash access. Exposed so `clear` and a failed mount share one path. */
void settings_defaults(struct wigwag_settings *s);

#endif /* SETTINGS_STORE_H */
