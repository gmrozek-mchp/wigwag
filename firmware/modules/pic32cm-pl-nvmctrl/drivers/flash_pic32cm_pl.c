/*
 * Flash (NVMCTRL) driver for Microchip PIC32CM PL.
 *
 * Written from the datasheet's own self-programming sequence (§26.4.2.3.4) rather than adapted from
 * an existing Microchip flash driver, because the existing ones drive a different peripheral. The
 * in-tree microchip,nvmctrl-g1-flash driver (module U2409) writes through a page buffer using the
 * PBC and EB commands; the PL revision has no page buffer and no such commands. Its command set is
 * FLWR / FLPER / FLMPER2..32 / LR / UR / CHER, and writes go 32 bits at a time straight into the
 * array. Enabling the g1 driver on this family would issue reserved commands.
 *
 * The sequence for every operation, quoting §26.4.2.3.4:
 *
 *   1. Confirm that any previous operation is completed by reading the INTFLAG.READY flag.
 *   2. Unprotect the NVMCTRL registers by clearing WPCTRL.WPEN.
 *   3. Write the desired command value to the CTRLB.CMD bit field.
 *   4. Write to the correct address in the array to start the operation.
 *   5. Wait for INTFLAG.READY to be set to confirm that the operation is done.
 *   6. Write the NOOP or NOCMD command to CTRLB.CMD to clear the current command.
 *   7. Protect the NVMCTRL registers by setting WPCTRL.WPEN.
 *
 * Two properties of this peripheral shape the code and are worth knowing before changing it:
 *
 * **The CPU stalls, it does not fault.** §26.4.2.3.1: "Reading any of the Flash sections while a
 * write or erase operation is in progress on that section will result in a bus wait, and the read
 * will be suspended until the ongoing operation is complete." So driving flash from code that lives
 * in flash is safe and needs no RAM-resident routine — which matters on a part with 8 KB of SRAM.
 * The cost is that the stall blocks *everything*, interrupts included, for the duration of the
 * operation. Measured on a PL10 at 24 MHz:
 *
 *   page erase (512 B)   10.1 ms      <- a hard blackout, one lamp frame's worth
 *   word write (4 B)      ~0.13 ms
 *
 * Our watchdog gives each task 500 ms (ADR-0016), so a single erase() call must stay under about
 * **49 pages (~24 KB)** or it will reboot the device mid-erase. Nothing here comes close — the
 * storage partition is 8 pages, 81 ms — but a caller that erased half of flash in one call would
 * find out the hard way, hence the number in writing.
 *
 * **Erase duration does not depend on the page count.** §26.4.2.3.3: "The duration of the erase
 * operation is the same for 1 page as for 32 pages." A multi-page erase is therefore strictly
 * cheaper than the equivalent loop, which is why FLMPER exists. Confirmed by measurement: 8 pages
 * one at a time costs 80.6 ms, almost exactly 8x a single page, so FLMPER8 would do the same work
 * in ~10 ms. This driver still erases one page at a time — see erase() for why that is deliberate
 * for now.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_pic32cm_pl_nvmctrl

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

LOG_MODULE_REGISTER(flash_pic32cm_pl, CONFIG_FLASH_LOG_LEVEL);

/*
 * Geometry comes from the chosen zephyr,flash node, so the base address is not duplicated between
 * this driver and the devicetree. Sizes come from PARAM at runtime instead (see init).
 */
#define FLASH_NODE	DT_CHOSEN(zephyr_flash)
#define FLASH_BASE	DT_REG_ADDR(FLASH_NODE)
#define FLASH_BYTES	DT_REG_SIZE(FLASH_NODE)

/* Writes are 32-bit; bytes and half-words are "discarded, and a bus error is returned" (§26.4.2.3.2). */
#define WRITE_BLOCK_SZ	4U
#define ERASED_BYTE	0xFFU

/*
 * Generous, and effectively unreachable: the bus stall means the operation has already finished by
 * the time the next instruction fetch completes. It exists so a peripheral that never sets READY
 * cannot wedge the caller — which on this device would mean the watchdog reboots us (ADR-0016), a
 * far better outcome than a silent hang, but better still to return -ETIMEDOUT and stay alive.
 */
#define OP_TIMEOUT_US	100000

struct flash_pl_config {
	nvmctrl_registers_t *regs;
};

struct flash_pl_data {
	struct k_mutex lock;
	struct flash_pages_layout layout;	/* single uniform layout: NVMP pages of PSZ bytes */
};

static const struct flash_parameters flash_pl_parameters = {
	.write_block_size = WRITE_BLOCK_SZ,
	.erase_value = ERASED_BYTE,
};

/*
 * WPCTRL is key-protected: a write without WPKEY is ignored. 0x4E564D is "NVM" in ASCII, the same
 * trick RSTC plays with "RST".
 */
static inline void wp_unprotect(nvmctrl_registers_t *regs)
{
	regs->NVMCTRL_WPCTRL = NVMCTRL_WPCTRL_WPKEY_KEY;
}

static inline void wp_protect(nvmctrl_registers_t *regs)
{
	regs->NVMCTRL_WPCTRL = NVMCTRL_WPCTRL_WPKEY_KEY | NVMCTRL_WPCTRL_WPEN(1);
}

static int wait_ready(nvmctrl_registers_t *regs)
{
	if (!WAIT_FOR((regs->NVMCTRL_INTFLAG & NVMCTRL_INTFLAG_READY_Msk) != 0U, OP_TIMEOUT_US,
		      k_busy_wait(1))) {
		LOG_ERR("NVMCTRL never became ready");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * Write CTRLB.CMD with the 0xA5 execution key, then wait for the controller to finish digesting it.
 *
 * **Every one of those steps is load-bearing.** The key must be written in the same store as the
 * command (§26.6.1). READY must already be 1 when the command is issued, and — the part that cost a
 * debugging round — issuing an *enable* command such as FLWR or FLPER is itself a command that
 * clears READY. Storing to the array before it lands sets STATUS.PROGE and the operation silently
 * does not happen: "STATUS.PROGE is also set if a previously written command has not yet completed."
 *
 * Measured on hardware: INTFLAG reads 0x00000000 immediately after writing FLPER. The first version
 * of this driver stored to the array on the next instruction and every erase failed with PROGE,
 * while writes appeared to work purely because a memcpy happened to sit in the gap. A sequence that
 * works by accident of instruction scheduling is worse than one that fails outright, so the wait is
 * inside this helper where it cannot be forgotten at a call site.
 */
static int issue_cmd(nvmctrl_registers_t *regs, uint32_t cmd)
{
	int ret = wait_ready(regs);

	if (ret != 0) {
		return ret;
	}

	regs->NVMCTRL_CTRLB = cmd | NVMCTRL_CTRLB_CMDEX_KEY;

	return wait_ready(regs);
}

/**
 * Consume and report the error flags.
 *
 * LOCKE and PROGE are distinguished deliberately. A lock error means the region is protected by
 * BOOTPROT or the LOCK bits and the operation was refused — a permissions answer, and once a
 * bootloader occupies the bottom of flash it is the *expected* answer for anything aimed there.
 * A programming error means the command itself was wrong or the block was busy. Collapsing both
 * into -EIO would turn "you may not write there" into "the flash is broken".
 */
static int take_error(nvmctrl_registers_t *regs)
{
	uint32_t status = regs->NVMCTRL_STATUS;
	int ret = 0;

	if ((status & NVMCTRL_STATUS_LOCKE_Msk) != 0U) {
		LOG_ERR("lock error: region protected by BOOTPROT or LOCK");
		ret = -EACCES;
	} else if ((status & NVMCTRL_STATUS_PROGE_Msk) != 0U) {
		LOG_ERR("programming error");
		ret = -EIO;
	}

	/* Write-1-to-clear, so the next operation reports its own errors rather than inheriting. */
	regs->NVMCTRL_STATUS = status & (NVMCTRL_STATUS_LOCKE_Msk | NVMCTRL_STATUS_PROGE_Msk);
	regs->NVMCTRL_INTFLAG = NVMCTRL_INTFLAG_ERROR_Msk;

	return ret;
}

static bool range_ok(off_t offset, size_t len)
{
	/* Signed offset from the API, unsigned length; check both ends without overflowing. */
	if (offset < 0 || len > FLASH_BYTES) {
		return false;
	}

	return (size_t)offset <= FLASH_BYTES - len;
}

static int flash_pl_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	ARG_UNUSED(dev);

	if (!range_ok(offset, len)) {
		return -EINVAL;
	}

	if (len == 0U) {
		return 0;
	}

	/*
	 * Plain memory read: the array is memory-mapped and reads may be of byte, half-word or word
	 * size (§26.4.2.3.1). No lock needed — a read concurrent with a write on the same section
	 * simply stalls until the write finishes, which is the hardware doing the synchronisation for
	 * us.
	 */
	memcpy(data, (const void *)(FLASH_BASE + offset), len);

	return 0;
}

static int flash_pl_write(const struct device *dev, off_t offset, const void *data, size_t len)
{
	const struct flash_pl_config *cfg = dev->config;
	struct flash_pl_data *dat = dev->data;
	nvmctrl_registers_t *regs = cfg->regs;
	const uint8_t *src = data;
	int ret;

	if (!range_ok(offset, len)) {
		return -EINVAL;
	}

	if (((size_t)offset % WRITE_BLOCK_SZ) != 0U || (len % WRITE_BLOCK_SZ) != 0U) {
		return -EINVAL;
	}

	if (len == 0U) {
		return 0;
	}

	k_mutex_lock(&dat->lock, K_FOREVER);

	ret = wait_ready(regs);
	if (ret != 0) {
		goto out;
	}

	(void)take_error(regs);	/* clear anything stale before we attribute errors to ourselves */

	wp_unprotect(regs);

	/*
	 * FLWR stays enabled across the whole transfer: "Several writes can be done while the Flash
	 * write mode is enabled" (§26.4.2.3.4.1). Each word still has to complete before the next,
	 * so READY is polled per word rather than once at the end.
	 */
	ret = issue_cmd(regs, NVMCTRL_CTRLB_CMD_FLWR);
	if (ret != 0) {
		goto restore;
	}

	for (size_t done = 0; done < len; done += WRITE_BLOCK_SZ) {
		uint32_t word;

		/*
		 * memcpy rather than a cast: callers pass arbitrary buffers, and the settings and NVS
		 * subsystems in particular hand over pointers into packed structures. An unaligned
		 * 32-bit load on this core is an unconditional hard fault.
		 */
		memcpy(&word, src + done, sizeof(word));

		*(volatile uint32_t *)(FLASH_BASE + offset + done) = word;

		ret = wait_ready(regs);
		if (ret != 0) {
			break;
		}

		ret = take_error(regs);
		if (ret != 0) {
			break;
		}
	}

restore:
	(void)issue_cmd(regs, NVMCTRL_CTRLB_CMD_NOCMD);
	wp_protect(regs);

out:
	k_mutex_unlock(&dat->lock);

	return ret;
}

static int flash_pl_erase(const struct device *dev, off_t offset, size_t size)
{
	const struct flash_pl_config *cfg = dev->config;
	struct flash_pl_data *dat = dev->data;
	nvmctrl_registers_t *regs = cfg->regs;
	const size_t page_sz = dat->layout.pages_size;
	int ret;

	if (!range_ok(offset, size)) {
		return -EINVAL;
	}

	if (((size_t)offset % page_sz) != 0U || (size % page_sz) != 0U) {
		return -EINVAL;
	}

	if (size == 0U) {
		return 0;
	}

	k_mutex_lock(&dat->lock, K_FOREVER);

	ret = wait_ready(regs);
	if (ret != 0) {
		goto out;
	}

	(void)take_error(regs);

	wp_unprotect(regs);

	/*
	 * One page at a time, even though FLMPER2..32 would erase up to 32 pages for the price of
	 * one (§26.4.2.3.3) — a real saving this deliberately declines for now.
	 *
	 * Multi-page erase carries a constraint that single-page does not: "All pages that are
	 * erased at once must be within a single logical NVM section; Boot or application code. If
	 * the number of pages selected crosses the boundary between two sections, the operation is
	 * aborted and STATUS.LOCKE is set." That boundary is wherever BOOTPROT happens to be, which
	 * is a fuse this driver cannot see and which a future bootloader will move. Getting it wrong
	 * fails as a lock error on a legitimate erase.
	 *
	 * The saving is real and measured — 80.6 ms for the 8-page storage partition versus ~10 ms if
	 * done as one FLMPER8 — but the use here is a 4 KB partition written occasionally, and 81 ms
	 * is a sixth of the watchdog's budget with nothing else contending. Revisit with FLMPER if
	 * something needs to erase enough flash to approach the ~49-page ceiling, and gate it on
	 * reading BOOTPROT so a multi-page erase cannot straddle the boot/application boundary.
	 */
	for (size_t done = 0; done < size; done += page_sz) {
		/*
		 * Re-enabled per page rather than once outside the loop. §26.6.1: "A change from one
		 * command to another should always go through NOCMD or NOOP" — and re-issuing the same
		 * command back to back is a change as far as PROGE is concerned, because the previous
		 * one has not completed.
		 */
		ret = issue_cmd(regs, NVMCTRL_CTRLB_CMD_FLPER);
		if (ret != 0) {
			break;
		}

		/* The store is what starts the erase; the value written is irrelevant. */
		*(volatile uint32_t *)(FLASH_BASE + offset + done) = 0xFFFFFFFFU;

		ret = wait_ready(regs);
		if (ret != 0) {
			break;
		}

		ret = take_error(regs);
		if (ret != 0) {
			break;
		}

		ret = issue_cmd(regs, NVMCTRL_CTRLB_CMD_NOCMD);
		if (ret != 0) {
			break;
		}
	}

	(void)issue_cmd(regs, NVMCTRL_CTRLB_CMD_NOCMD);
	wp_protect(regs);

out:
	k_mutex_unlock(&dat->lock);

	return ret;
}

static const struct flash_parameters *flash_pl_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &flash_pl_parameters;
}

static int flash_pl_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);

	*size = FLASH_BYTES;

	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void flash_pl_page_layout(const struct device *dev,
				 const struct flash_pages_layout **layout, size_t *layout_size)
{
	struct flash_pl_data *dat = dev->data;

	/* Uniform pages, so exactly one layout entry describes the whole device. */
	*layout = &dat->layout;
	*layout_size = 1;
}
#endif /* CONFIG_FLASH_PAGE_LAYOUT */

static int flash_pl_init(const struct device *dev)
{
	const struct flash_pl_config *cfg = dev->config;
	struct flash_pl_data *dat = dev->data;
	uint32_t param = cfg->regs->NVMCTRL_PARAM;
	size_t pages = (param & NVMCTRL_PARAM_NVMP_Msk) >> NVMCTRL_PARAM_NVMP_Pos;
	size_t page_sz = 8U << ((param & NVMCTRL_PARAM_PSZ_Msk) >> NVMCTRL_PARAM_PSZ_Pos);

	k_mutex_init(&dat->lock);

	/*
	 * Geometry from the hardware, then checked against the devicetree rather than trusted. If
	 * they disagree, something is wrong about which part this image was built for, and silently
	 * believing one of them is how a driver ends up erasing the wrong page. This also catches the
	 * reg address pointing at the wrong peripheral, since PARAM would read as nonsense.
	 */
	if (pages == 0U || page_sz == 0U || (pages * page_sz) != FLASH_BYTES) {
		LOG_ERR("PARAM says %zu pages of %zu bytes (%zu), devicetree says %zu", pages,
			page_sz, pages * page_sz, (size_t)FLASH_BYTES);
		return -ENODEV;
	}

	dat->layout.pages_count = pages;
	dat->layout.pages_size = page_sz;

	/* Leave the registers protected; every operation unprotects and re-protects around itself. */
	wp_protect(cfg->regs);

	LOG_INF("PIC32CM PL flash: %zu pages of %zu bytes at 0x%lx", pages, page_sz,
		(unsigned long)FLASH_BASE);

	return 0;
}

static DEVICE_API(flash, flash_pl_api) = {
	.read = flash_pl_read,
	.write = flash_pl_write,
	.erase = flash_pl_erase,
	.get_parameters = flash_pl_get_parameters,
	.get_size = flash_pl_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = flash_pl_page_layout,
#endif
};

#define FLASH_PL_INIT(n)                                                                           \
	static const struct flash_pl_config flash_pl_config_##n = {                                \
		.regs = (nvmctrl_registers_t *)DT_INST_REG_ADDR(n),                                \
	};                                                                                         \
	static struct flash_pl_data flash_pl_data_##n;                                             \
	DEVICE_DT_INST_DEFINE(n, flash_pl_init, NULL, &flash_pl_data_##n, &flash_pl_config_##n,    \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &flash_pl_api);

DT_INST_FOREACH_STATUS_OKAY(FLASH_PL_INIT)
