/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2011-2014, Xiaomi Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <app.h>
#include <debug.h>
#include <arch/arm.h>
#include <dev/udc.h>
#include <string.h>
#include <limits.h>
#include <kernel/thread.h>
#include <arch/ops.h>

#include <dev/flash.h>
#include <lib/ptable.h>
#include <dev/keys.h>
#include <dev/fbcon.h>
#include <baseband.h>
#include <target.h>
#include <mmc.h>
#include <partition_parser.h>
#include <platform.h>
#include <crypto_hash.h>
#include <smem.h>

#if DEVICE_TREE
#include <libfdt.h>
#endif

#include "image_verify.h"
#include "recovery.h"
#include "bootimg.h"
#include "fastboot.h"
#include "sparse_format.h"
#include "mmc.h"
#include "devinfo.h"
#include "board.h"

#include "scm.h"
#include "aboot.h"

/* fastboot command function pointer */
typedef void (*fastboot_cmd_fn) (const char *, void *, unsigned);

struct fastboot_cmd_desc {
	char * name;
	fastboot_cmd_fn cb;
};

#define EXPAND(NAME) #NAME
#define TARGET(NAME) EXPAND(NAME)
#define DEFAULT_CMDLINE "mem=100M console=null";

#ifdef MEMBASE
#define EMMC_BOOT_IMG_HEADER_ADDR (0xFF000+(MEMBASE))
#else
#define EMMC_BOOT_IMG_HEADER_ADDR 0xFF000
#endif

#ifndef MEMSIZE
#define MEMSIZE 1024*1024
#endif

#define MAX_TAGS_SIZE   1024

#define RECOVERY_MODE   0x77665502
#define FASTBOOT_MODE   0x77665500

#if DEVICE_TREE
#define DEV_TREE_SUCCESS        0
#define DEV_TREE_MAGIC          "QCDT"
#define DEV_TREE_VERSION        1
#define DEV_TREE_HEADER_SIZE    12


struct dt_entry{
	uint32_t platform_id;
	uint32_t variant_id;
	uint32_t soc_rev;
	uint32_t offset;
	uint32_t size;
};

struct dt_table{
	uint32_t magic;
	uint32_t version;
	unsigned num_entries;
};

struct dt_entry * get_device_tree_ptr(struct dt_table *);
int update_device_tree(const void *, char *, void *, unsigned);
#endif

#define ADD_OF(a, b) (UINT_MAX - b > a) ? (a + b) : UINT_MAX

static const char *emmc_cmdline = " androidboot.emmc=true";
static const char *usb_sn_cmdline = " androidboot.serialno=";
static const char *battchg_pause = " androidboot.mode=charger";
static const char *auth_kernel = " androidboot.authorized_kernel=true";

static const char *baseband_apq     = " androidboot.baseband=apq";
static const char *baseband_msm     = " androidboot.baseband=msm";
static const char *baseband_csfb    = " androidboot.baseband=csfb";
static const char *baseband_svlte2a = " androidboot.baseband=svlte2a";
static const char *baseband_mdm     = " androidboot.baseband=mdm";
static const char *baseband_sglte   = " androidboot.baseband=sglte";
static const char *baseband_dsda    = " androidboot.baseband=dsda";
static const char *baseband_dsda2   = " androidboot.baseband=dsda2";
static const char *baseband_sglte2  = " androidboot.baseband=sglte2";

static const char *earlyprintk = DEBUGLEVEL > 1 ? " earlyprintk" : "";

unsigned pu_reason = 0;
unsigned reboot_mode = 0;
unsigned fastboot_reason = 0;

/* keep them same length */
static const char *boot0_cmdline = " syspart=system ";
static const char *boot1_cmdline = " syspart=system1";

enum {
	DUALBOOT_BOOT_NONE,
	DUALBOOT_BOOT_FIRST,
	DUALBOOT_BOOT_SECOND,
};
static unsigned dual_boot_sign = DUALBOOT_BOOT_NONE;

/* Assuming unauthorized kernel image by default */
static int auth_kernel_img = 0;

static device_info device = {DEVICE_MAGIC, 0, 0};

static struct udc_device surf_udc_device = {
	.vendor_id	= 0x18d1,
	.product_id	= 0xD00D,
	.version_id	= 0x0100,
	.manufacturer	= "Google",
	.product	= "Android",
};

struct atag_ptbl_entry
{
	char name[16];
	unsigned offset;
	unsigned size;
	unsigned flags;
};

char sn_buf[13];

extern int emmc_recovery_init(void);

#if NO_KEYPAD_DRIVER
extern int fastboot_trigger(void);
#endif

static void ptentry_to_tag(unsigned **ptr, struct ptentry *ptn)
{
	struct atag_ptbl_entry atag_ptn;

	memcpy(atag_ptn.name, ptn->name, 16);
	atag_ptn.name[15] = '\0';
	atag_ptn.offset = ptn->start;
	atag_ptn.size = ptn->length;
	atag_ptn.flags = ptn->flags;
	memcpy(*ptr, &atag_ptn, sizeof(struct atag_ptbl_entry));
	*ptr += sizeof(struct atag_ptbl_entry) / sizeof(unsigned);
}

unsigned char *update_cmdline(const char * cmdline)
{
	int cmdline_len = 0;
	int have_cmdline = 0;
	unsigned char *cmdline_final = NULL;
	int pause_at_bootup = 0;

	if (cmdline && cmdline[0]) {
		cmdline_len = strlen(cmdline);
		have_cmdline = 1;
	}
	if (target_is_emmc_boot()) {
		cmdline_len += strlen(emmc_cmdline);
	}

	cmdline_len += strlen(usb_sn_cmdline);
	cmdline_len += strlen(sn_buf);
	cmdline_len += strlen(earlyprintk);

	if (target_pause_for_battery_charge()) {
		pause_at_bootup = 1;
		cmdline_len += strlen(battchg_pause);
	}

	/* bootX_cmdline is a mandatory parameter
	 * it should be there in any case
	 */
	cmdline_len += strlen(boot0_cmdline);

	if(target_use_signed_kernel() && auth_kernel_img) {
		cmdline_len += strlen(auth_kernel);
	}

	/* Determine correct androidboot.baseband to use */
	switch(target_baseband())
	{
		case BASEBAND_APQ:
			cmdline_len += strlen(baseband_apq);
			break;

		case BASEBAND_MSM:
			cmdline_len += strlen(baseband_msm);
			break;

		case BASEBAND_CSFB:
			cmdline_len += strlen(baseband_csfb);
			break;

		case BASEBAND_SVLTE2A:
			cmdline_len += strlen(baseband_svlte2a);
			break;

		case BASEBAND_MDM:
			cmdline_len += strlen(baseband_mdm);
			break;

		case BASEBAND_SGLTE:
			cmdline_len += strlen(baseband_sglte);
			break;

		case BASEBAND_SGLTE2:
			cmdline_len += strlen(baseband_sglte2);
			break;

		case BASEBAND_DSDA:
			cmdline_len += strlen(baseband_dsda);
			break;

		case BASEBAND_DSDA2:
			cmdline_len += strlen(baseband_dsda2);
			break;
	}

	if (cmdline_len > 0) {
		const char *src;
		char *dst = malloc((cmdline_len + 4) & (~3));
		assert(dst != NULL);

		/* Save start ptr for debug print */
		cmdline_final = dst;
		if (have_cmdline) {
			src = cmdline;
			while ((*dst++ = *src++));
		}
		if (target_is_emmc_boot()) {
			src = emmc_cmdline;
			if (have_cmdline) --dst;
			have_cmdline = 1;
			while ((*dst++ = *src++));
		}

		src = usb_sn_cmdline;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));
		src = sn_buf;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));
		src = earlyprintk;
		if (have_cmdline) --dst;
		have_cmdline = 1;
		while ((*dst++ = *src++));

		if (pause_at_bootup) {
			src = battchg_pause;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		if (dual_boot_sign == DUALBOOT_BOOT_SECOND) {
			src = boot1_cmdline;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		} else {
			if (dual_boot_sign == DUALBOOT_BOOT_NONE)
				dprintf(CRITICAL, "ERROR: boot and system not chosen\n");
			src = boot0_cmdline;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		if(target_use_signed_kernel() && auth_kernel_img) {
			src = auth_kernel;
			if (have_cmdline) --dst;
			while ((*dst++ = *src++));
		}

		switch(target_baseband())
		{
			case BASEBAND_APQ:
				src = baseband_apq;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MSM:
				src = baseband_msm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_CSFB:
				src = baseband_csfb;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SVLTE2A:
				src = baseband_svlte2a;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_MDM:
				src = baseband_mdm;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SGLTE:
				src = baseband_sglte;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_SGLTE2:
				src = baseband_sglte2;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_DSDA:
				src = baseband_dsda;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;

			case BASEBAND_DSDA2:
				src = baseband_dsda2;
				if (have_cmdline) --dst;
				while ((*dst++ = *src++));
				break;
		}
	}
	return cmdline_final;
}

unsigned *atag_core(unsigned *ptr)
{
	/* CORE */
	*ptr++ = 2;
	*ptr++ = 0x54410001;

	return ptr;

}

unsigned *atag_ramdisk(unsigned *ptr, void *ramdisk,
							   unsigned ramdisk_size)
{
	if (ramdisk_size) {
		*ptr++ = 4;
		*ptr++ = 0x54420005;
		*ptr++ = (unsigned)ramdisk;
		*ptr++ = ramdisk_size;
	}

	return ptr;
}

unsigned *atag_ptable(unsigned **ptr_addr)
{
	int i;
	struct ptable *ptable;

	if ((ptable = flash_get_ptable()) && (ptable->count != 0)) {
        	*(*ptr_addr)++ = 2 + (ptable->count * (sizeof(struct atag_ptbl_entry) /
					  		sizeof(unsigned)));
		*(*ptr_addr)++ = 0x4d534d70;
		for (i = 0; i < ptable->count; ++i)
			ptentry_to_tag(ptr_addr, ptable_get(ptable, i));
	}

	return (*ptr_addr);
}

unsigned *atag_cmdline(unsigned *ptr, const char *cmdline)
{
	int cmdline_length = 0;
	int n;
	char *dest;

	cmdline_length = strlen((const char*)cmdline);
	n = (cmdline_length + 4) & (~3);

	*ptr++ = (n / 4) + 2;
	*ptr++ = 0x54410009;
	dest = (char *) ptr;
	while ((*dest++ = *cmdline++));
	ptr += (n / 4);

	return ptr;
}

unsigned *atag_end(unsigned *ptr)
{
	/* END */
	*ptr++ = 0;
	*ptr++ = 0;

	return ptr;
}

void generate_atags(unsigned *ptr, const char *cmdline,
                    void *ramdisk, unsigned ramdisk_size)
{

	ptr = atag_core(ptr);
	ptr = atag_ramdisk(ptr, ramdisk, ramdisk_size);
	ptr = target_atag_mem(ptr);
	ptr = target_atag_pureason(ptr);
	ptr = target_atag_hwversion(ptr);
	ptr = target_atag_memvendor(ptr);

	/* Skip NAND partition ATAGS for eMMC boot */
	if (!target_is_emmc_boot()){
		ptr = atag_ptable(&ptr);
	}

	ptr = atag_cmdline(ptr, cmdline);
	ptr = atag_end(ptr);
}

typedef void entry_func_ptr(unsigned, unsigned, unsigned*);
void boot_linux(void *kernel, unsigned *tags,
		const char *cmdline, unsigned machtype,
		void *ramdisk, unsigned ramdisk_size)
{
	unsigned char *final_cmdline;
#if DEVICE_TREE
	int ret = 0;
#endif

	void (*entry)(unsigned, unsigned, unsigned*) = (entry_func_ptr*)(PA((addr_t)kernel));
	uint32_t tags_phys = PA((addr_t)tags);

	ramdisk = PA(ramdisk);

	final_cmdline = update_cmdline((const char*)cmdline);

#if DEVICE_TREE
	dprintf(INFO, "Updating device tree: start\n");

	/* Update the Device Tree */
	ret = update_device_tree((void *)tags, final_cmdline, ramdisk, ramdisk_size);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Updating Device Tree Failed \n");
		ASSERT(0);
	}

	dprintf(INFO, "Updating device tree: done\n");
#else
	/* Generating the Atags */
	generate_atags(tags, final_cmdline, ramdisk, ramdisk_size);
#endif

	dprintf(INFO, "booting linux @ %p, ramdisk @ %p (%d)\n",
		entry, ramdisk, ramdisk_size);

	enter_critical_section();
	/* do any platform specific cleanup before kernel entry */
	platform_uninit();
	arch_disable_cache(UCACHE);
	/* NOTE:
	 * The value of "entry" is getting corrupted at this point.
	 * The value is in R4 and gets pushed to stack on entry into
	 * disable_cache(), however, on return it is not the same.
	 * Not entirely sure why this dsb() seems to take of this.
	 * The stack pop operation on return from disable_cache()
	 * should restore R4 properly, but that is not happening.
	 * Will need to revisit to find the root cause.
	 */
	dsb();
#if ARM_WITH_MMU
	arch_disable_mmu();
#endif

	entry(0, machtype, (unsigned*)tags_phys);
}

unsigned page_size = 0;
unsigned page_mask = 0;
/* Function to check if the memory address range falls within the aboot
 * boundaries.
 * start: Start of the memory region
 * size: Size of the memory region
 */
int check_aboot_addr_range_overlap(uint32_t start, uint32_t size)
{
	/* Check for boundary conditions. */
	if ((UINT_MAX - start) < size)
		return -1;

	/* Check for memory overlap. */
	if ((start < MEMBASE) && ((start + size) <= MEMBASE))
		return 0;
	else if (start > (MEMBASE + MEMSIZE))
		return 0;
	else
		return -1;
}

#define ROUND_TO_PAGE(x,y) (((x) + (y)) & (~(y)))

static unsigned char buf[4096]; //Equal to max-supported pagesize
#if DEVICE_TREE
static unsigned char dt_buf[4096];
#endif
#define DUALBOOT_OFFSET_SECTORS 8	/* 4 pages, for recovery_message */

static void check_boot_image(void)
{
	int index = INVALID_PTN;
	unsigned long long ptn = 0;
	struct dual_boot_message *dualboot_msg;

	if (dual_boot_sign != DUALBOOT_BOOT_NONE)
		goto out;

	dual_boot_sign = DUALBOOT_BOOT_FIRST;

	index = partition_get_index("misc");
	ptn = partition_get_offset(index);
	if (ptn == 0) {
		dprintf(CRITICAL, "ERROR: No misc found\n");
		goto out;
	}

	ptn += DUALBOOT_OFFSET_SECTORS * MMC_BOOT_RD_BLOCK_LEN;
	memset(buf, 0, sizeof(buf));
	if (mmc_read(ptn, (unsigned int *)buf, MMC_BOOT_RD_BLOCK_LEN)) {
		dprintf(CRITICAL, "ERROR: Cannot read misc\n");
		goto out;
	}

	dualboot_msg = (struct dual_boot_message *)buf;

	if (!strncmp(dualboot_msg->command, "boot-system", 11)) {
		if (dualboot_msg->command[11] == '1')
			dual_boot_sign = DUALBOOT_BOOT_SECOND;
	} else
		dprintf(CRITICAL, "WARNING: dual_boot_message not set\n");

out:
	/* either DUALBOOT_BOOT_FIRST or DUALBOOT_BOOT_SECOND at this point */
	dprintf(INFO, "Dualboot choosing: %d of (1, 2) boot & system\n",
		dual_boot_sign);
}

static void verify_signed_bootimg(uint32_t bootimg_addr, uint32_t bootimg_size)
{
	int ret;

	/* Assume device is rooted at this time. */
	device.is_tampered = 1;

	dprintf(INFO, "Authenticating boot image (%d): start\n", bootimg_size);

	ret = image_verify((unsigned char *)bootimg_addr,
					   (unsigned char *)(bootimg_addr + bootimg_size),
					   bootimg_size,
					   CRYPTO_AUTH_ALG_SHA256);

	dprintf(INFO, "Authenticating boot image: done return value = %d\n", ret);

	if (ret)
	{
		/* Authorized kernel */
		device.is_tampered = 0;
	}

#if USE_PCOM_SECBOOT
	set_tamper_flag(device.is_tampered);
#endif

	if(device.is_tampered)
	{
		write_device_info_mmc(&device);
	#ifdef TZ_TAMPER_FUSE
		set_tamper_fuse_cmd();
	#endif
	#ifdef ASSERT_ON_TAMPER
		dprintf(CRITICAL, "Device is tampered. Asserting..\n");
		ASSERT(0);
	#endif
	}
}

int boot_linux_from_mmc(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	struct boot_img_hdr *uhdr;
	unsigned offset = 0;
	unsigned long long ptn = 0;
	const char *cmdline;
	int index = INVALID_PTN;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;
	unsigned second_actual = 0;

#if DEVICE_TREE
	struct dt_table *table;
	struct dt_entry *dt_entry_ptr;
	unsigned dt_table_offset;
#endif

	uhdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
	if (!memcmp(uhdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(INFO, "Unified boot method!\n");
		hdr = uhdr;
		goto unified_boot;
	}
	if (!boot_into_recovery) {
		check_boot_image();

		if (dual_boot_sign == DUALBOOT_BOOT_SECOND)
			index = partition_get_index("boot1");
		else
			index = partition_get_index("boot");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No boot partition found\n");
                    return -1;
		}
	}
	else {
		index = partition_get_index("recovery");
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			dprintf(CRITICAL, "ERROR: No recovery partition found\n");
                    return -1;
		}
	}

	if (mmc_read(ptn + offset, (unsigned int *) buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
                return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
                return -1;
	}

	if (hdr->page_size && (hdr->page_size != page_size)) {
		page_size = hdr->page_size;
		page_mask = page_size - 1;
	}

	/* Get virtual addresses since the hdr saves physical addresses. */
	hdr->kernel_addr = VA((addr_t)(hdr->kernel_addr));
	hdr->ramdisk_addr = VA((addr_t)(hdr->ramdisk_addr));
	hdr->tags_addr = VA((addr_t)(hdr->tags_addr));

	kernel_actual  = ROUND_TO_PAGE(hdr->kernel_size,  page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);

	/* Check if the addresses in the header are valid. */
	if (check_aboot_addr_range_overlap(hdr->kernel_addr, kernel_actual) ||
		check_aboot_addr_range_overlap(hdr->ramdisk_addr, ramdisk_actual))
	{
		dprintf(CRITICAL, "kernel/ramdisk addresses overlap with aboot addresses.\n");
		return -1;
	}

#ifndef DEVICE_TREE
	if (check_aboot_addr_range_overlap(hdr->tags_addr, MAX_TAGS_SIZE))
	{
		dprintf(CRITICAL, "Tags addresses overlap with aboot addresses.\n");
		return -1;
	}
#endif

	/* Authenticate Kernel */
	dprintf(INFO, "use_signed_kernel=%d, is_unlocked=%d, is_tampered=%d.\n",
		(int) target_use_signed_kernel(),
		device.is_unlocked,
		device.is_tampered);

	if(target_use_signed_kernel() && (!device.is_unlocked))
	{
		offset = 0;

		image_addr = (unsigned char *)target_get_scratch_address();

#if DEVICE_TREE
		dt_actual = ROUND_TO_PAGE(hdr->dt_size, page_mask);
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual + dt_actual);

		if (check_aboot_addr_range_overlap(hdr->tags_addr, hdr->dt_size))
		{
			dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
			return -1;
		}
#else
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual);
#endif

		dprintf(INFO, "Loading boot image (%d): start\n", imagesize_actual);

		dprintf(INFO, "Loading boot image (%d): start\n", imagesize_actual);

		if (check_aboot_addr_range_overlap(image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "Boot image buffer address overlaps with aboot addresses.\n");
			return -1;
		}

		/* Read image without signature */
		if (mmc_read(ptn + offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		dprintf(INFO, "Loading boot image (%d): done\n", imagesize_actual);

		offset = imagesize_actual;

		if (check_aboot_addr_range_overlap(image_addr + offset, page_size))
		{
			dprintf(CRITICAL, "Signature read buffer address overlaps with aboot addresses.\n");
			return -1;
		}

		/* Read signature */
		if(mmc_read(ptn + offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
			return -1;
		}

		verify_signed_bootimg(image_addr, imagesize_actual);

		/* Move kernel, ramdisk and device tree to correct address */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);

		#if DEVICE_TREE
		if(hdr->dt_size) {
			table = (struct dt_table*) dt_buf;
			dt_table_offset = (image_addr + page_size + kernel_actual + ramdisk_actual + second_actual);

			memmove((void *) dt_buf, (char *)dt_table_offset, page_size);

			/* Restriction that the device tree entry table should be less than a page*/
			ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

			/* Validate the device tree table header */
			if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
				dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
				return -1;
			}

			/* Find index of device tree within device tree table */
			if((dt_entry_ptr = get_device_tree_ptr(table)) == NULL){
				dprintf(CRITICAL, "ERROR: Device Tree Blob cannot be found\n");
				return -1;
			}

			/* Validate and Read device device tree in the "tags_add */
			if (check_aboot_addr_range_overlap(hdr->tags_addr, dt_entry_ptr->size))
			{
				dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
				return -1;
			}

			memmove((void *)hdr->tags_addr, (char *)dt_table_offset + dt_entry_ptr->offset, dt_entry_ptr->size);
		}
		#endif
	}
	else
	{
		second_actual  = ROUND_TO_PAGE(hdr->second_size,  page_mask);

		dprintf(INFO, "Loading boot image (%d): start\n",
				kernel_actual + ramdisk_actual);

		offset = page_size;

		/* Load kernel */
		if (mmc_read(ptn + offset, (void *)hdr->kernel_addr, kernel_actual)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
					return -1;
		}
		offset += kernel_actual;

		/* Load ramdisk */
		if(ramdisk_actual != 0)
		{
			if (mmc_read(ptn + offset, (void *)hdr->ramdisk_addr, ramdisk_actual)) {
				dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
				return -1;
			}
		}
		offset += ramdisk_actual;

		dprintf(INFO, "Loading boot image (%d): done\n",
				kernel_actual + ramdisk_actual);

		if(hdr->second_size != 0) {
			offset += second_actual;
			/* Second image loading not implemented. */
			ASSERT(0);
		}

		#if DEVICE_TREE
		if(hdr->dt_size != 0) {

			/* Read the device tree table into buffer */
			if(mmc_read(ptn + offset,(unsigned int *) dt_buf, page_size)) {
				dprintf(CRITICAL, "ERROR: Cannot read the Device Tree Table\n");
				return -1;
			}
			table = (struct dt_table*) dt_buf;

			/* Restriction that the device tree entry table should be less than a page*/
			ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

			/* Validate the device tree table header */
			if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
				dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
				return -1;
			}

			/* Calculate the offset of device tree within device tree table */
			if((dt_entry_ptr = get_device_tree_ptr(table)) == NULL){
				dprintf(CRITICAL, "ERROR: Getting device tree address failed\n");
				return -1;
			}

			/* Validate and Read device device tree in the "tags_add */
			if (check_aboot_addr_range_overlap(hdr->tags_addr, dt_entry_ptr->size))
			{
				dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
				return -1;
			}

			if(mmc_read(ptn + offset + dt_entry_ptr->offset,
						 (void *)hdr->tags_addr, dt_entry_ptr->size)) {
				dprintf(CRITICAL, "ERROR: Cannot read device tree\n");
				return -1;
			}

			/* Validate the tags_addr */
			if (check_aboot_addr_range_overlap(hdr->tags_addr, kernel_actual))
			{
				dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
				return -1;
			}
		}
		#endif
	}

unified_boot:

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	boot_linux((void *)hdr->kernel_addr, (unsigned *) hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

int boot_linux_from_flash(void)
{
	struct boot_img_hdr *hdr = (void*) buf;
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned offset = 0;
	const char *cmdline;

	unsigned char *image_addr = 0;
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	unsigned imagesize_actual;
	unsigned second_actual;

	if (target_is_emmc_boot()) {
		hdr = (struct boot_img_hdr *)EMMC_BOOT_IMG_HEADER_ADDR;
		if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
			return -1;
		}
		goto continue_boot;
	}

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return -1;
	}

	if(!boot_into_recovery)
	{
	        ptn = ptable_find(ptable, "boot");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No boot partition found\n");
		        return -1;
	        }
	}
	else
	{
	        ptn = ptable_find(ptable, "recovery");
	        if (ptn == NULL) {
		        dprintf(CRITICAL, "ERROR: No recovery partition found\n");
		        return -1;
	        }
	}

	if (flash_read(ptn, offset, buf, page_size)) {
		dprintf(CRITICAL, "ERROR: Cannot read boot image header\n");
		return -1;
	}

	if (memcmp(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
		dprintf(CRITICAL, "ERROR: Invalid boot image header\n");
		return -1;
	}

	if (hdr->page_size != page_size) {
		dprintf(CRITICAL, "ERROR: Invalid boot image pagesize. Device pagesize: %d, Image pagesize: %d\n",page_size,hdr->page_size);
		return -1;
	}

	/* Get virtual addresses since the hdr saves physical addresses. */
	hdr->kernel_addr = VA((addr_t)(hdr->kernel_addr));
	hdr->ramdisk_addr = VA((addr_t)(hdr->ramdisk_addr));
	hdr->tags_addr = VA((addr_t)(hdr->tags_addr));

	kernel_actual  = ROUND_TO_PAGE(hdr->kernel_size,  page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);

	/* Check if the addresses in the header are valid. */
	if (check_aboot_addr_range_overlap(hdr->kernel_addr, kernel_actual) ||
		check_aboot_addr_range_overlap(hdr->ramdisk_addr, ramdisk_actual))
	{
		dprintf(CRITICAL, "kernel/ramdisk addresses overlap with aboot addresses.\n");
		return -1;
	}

#ifndef DEVICE_TREE
		if (check_aboot_addr_range_overlap(hdr->tags_addr, MAX_TAGS_SIZE))
		{
			dprintf(CRITICAL, "Tags addresses overlap with aboot addresses.\n");
			return -1;
		}
#endif

	/* Authenticate Kernel */
	if(target_use_signed_kernel() && (!device.is_unlocked))
	{
		image_addr = (unsigned char *)target_get_scratch_address();
		offset = 0;

#if DEVICE_TREE
		dt_actual = ROUND_TO_PAGE(hdr->dt_size, page_mask);
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual + dt_actual);

		if (check_aboot_addr_range_overlap(hdr->tags_addr, hdr->dt_size))
		{
			dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
			return -1;
		}
#else
		imagesize_actual = (page_size + kernel_actual + ramdisk_actual);
#endif

		dprintf(INFO, "Loading boot image (%d): start\n", imagesize_actual);

		/* Read image without signature */
		if (flash_read(ptn, offset, (void *)image_addr, imagesize_actual))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image\n");
				return -1;
		}

		dprintf(INFO, "Loading boot image (%d): done\n", imagesize_actual);

		offset = imagesize_actual;
		/* Read signature */
		if (flash_read(ptn, offset, (void *)(image_addr + offset), page_size))
		{
			dprintf(CRITICAL, "ERROR: Cannot read boot image signature\n");
			return -1;
		}

		verify_signed_bootimg(image_addr, imagesize_actual);

		/* Move kernel and ramdisk to correct address */
		memmove((void*) hdr->kernel_addr, (char *)(image_addr + page_size), hdr->kernel_size);
		memmove((void*) hdr->ramdisk_addr, (char *)(image_addr + page_size + kernel_actual), hdr->ramdisk_size);
#if DEVICE_TREE
		/* Validate and Read device device tree in the "tags_add */
		if (check_aboot_addr_range_overlap(hdr->tags_addr, dt_entry_ptr->size))
		{
			dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
			return -1;
		}

		memmove((void*) hdr->tags_addr, (char *)(image_addr + page_size + kernel_actual + ramdisk_actual), hdr->dt_size);
#endif

		/* Make sure everything from scratch address is read before next step!*/
		if(device.is_tampered)
		{
			write_device_info_flash(&device);
		}
#if USE_PCOM_SECBOOT
		set_tamper_flag(device.is_tampered);
#endif
	}
	else
	{
		offset = page_size;

		kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		second_actual = ROUND_TO_PAGE(hdr->second_size, page_mask);

		dprintf(INFO, "Loading boot image (%d): start\n",
				kernel_actual + ramdisk_actual);

		if (flash_read(ptn, offset, (void *)hdr->kernel_addr, kernel_actual)) {
			dprintf(CRITICAL, "ERROR: Cannot read kernel image\n");
			return -1;
		}
		offset += kernel_actual;

		if (flash_read(ptn, offset, (void *)hdr->ramdisk_addr, ramdisk_actual)) {
			dprintf(CRITICAL, "ERROR: Cannot read ramdisk image\n");
			return -1;
		}
		offset += ramdisk_actual;

		dprintf(INFO, "Loading boot image (%d): done\n",
				kernel_actual + ramdisk_actual);

		if(hdr->second_size != 0) {
			offset += second_actual;
			/* Second image loading not implemented. */
			ASSERT(0);
		}

#if DEVICE_TREE
		if(hdr->dt_size != 0) {

			/* Read the device tree table into buffer */
			if(flash_read(ptn, offset, (void *) dt_buf, page_size)) {
				dprintf(CRITICAL, "ERROR: Cannot read the Device Tree Table\n");
				return -1;
			}

			table = (struct dt_table*) dt_buf;

			/* Restriction that the device tree entry table should be less than a page*/
			ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

			/* Validate the device tree table header */
			if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
				dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
				return -1;
			}

			/* Calculate the offset of device tree within device tree table */
			if((dt_entry_ptr = dev_tree_get_entry_ptr(table)) == NULL){
				dprintf(CRITICAL, "ERROR: Getting device tree address failed\n");
				return -1;
			}

			/* Validate and Read device device tree in the "tags_add */
			if (check_aboot_addr_range_overlap(hdr->tags_addr, dt_entry_ptr->size))
			{
				dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
				return -1;
			}

			/* Read device device tree in the "tags_add */
			if(flash_read(ptn, offset + dt_entry_ptr->offset,
						 (void *)hdr->tags_addr, dt_entry_ptr->size)) {
				dprintf(CRITICAL, "ERROR: Cannot read device tree\n");
				return -1;
			}
		}
#endif

	}
continue_boot:

	if(hdr->cmdline[0]) {
		cmdline = (char*) hdr->cmdline;
	} else {
		cmdline = DEFAULT_CMDLINE;
	}
	dprintf(INFO, "cmdline = '%s'\n", cmdline);

	/* TODO: create/pass atags to kernel */

	boot_linux((void *)hdr->kernel_addr, (void *)hdr->tags_addr,
		   (const char *)cmdline, board_machtype(),
		   (void *)hdr->ramdisk_addr, hdr->ramdisk_size);

	return 0;
}

unsigned char info_buf[4096];
void write_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	memcpy(info, dev, sizeof(device_info));

	if(mmc_write((ptn + size - 512), 512, (void *)info_buf))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
		return;
	}
}

void read_device_info_mmc(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	unsigned long long ptn = 0;
	unsigned long long size;
	int index = INVALID_PTN;

	index = partition_get_index("aboot");
	ptn = partition_get_offset(index);
	if(ptn == 0)
	{
		return;
	}

	size = partition_get_size(index);

	if(mmc_read((ptn + size - 512), (void *)info_buf, 512))
	{
		dprintf(CRITICAL, "ERROR: Cannot read device info\n");
		return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;

		write_device_info_mmc(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info_flash(device_info *dev)
{
	struct device_info *info = (void *) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	memcpy(info, dev, sizeof(device_info));

	if (flash_write(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}
}

void read_device_info_flash(device_info *dev)
{
	struct device_info *info = (void*) info_buf;
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL)
	{
		dprintf(CRITICAL, "ERROR: Partition table not found\n");
		return;
	}

	ptn = ptable_find(ptable, "devinfo");
	if (ptn == NULL)
	{
		dprintf(CRITICAL, "ERROR: No boot partition found\n");
			return;
	}

	if (flash_read(ptn, 0, (void *)info_buf, page_size))
	{
		dprintf(CRITICAL, "ERROR: Cannot write device info\n");
			return;
	}

	if (memcmp(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE))
	{
		memcpy(info->magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
		info->is_unlocked = 0;
		info->is_tampered = 0;
		write_device_info_flash(info);
	}
	memcpy(dev, info, sizeof(device_info));
}

void write_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		write_device_info_mmc(dev);
	}
	else
	{
		write_device_info_flash(dev);
	}
}

void read_device_info(device_info *dev)
{
	if(target_is_emmc_boot())
	{
		read_device_info_mmc(dev);
	}
	else
	{
		read_device_info_flash(dev);
	}
}

void reset_device_info()
{
	dprintf(ALWAYS, "reset_device_info called.");
	device.is_tampered = 0;
	write_device_info(&device);
}

void set_device_root()
{
	dprintf(ALWAYS, "set_device_root called.");
	device.is_tampered = 1;
	write_device_info(&device);
}

#if DEVICE_TREE
int copy_dtb(uint8_t *boot_image_start)
{
	uint32 dt_image_offset = 0;
	uint32_t n;
	struct dt_table *table;
	struct dt_entry *dt_entry_ptr;
	unsigned dt_table_offset;

	struct boot_img_hdr *hdr = (struct boot_img_hdr *) (boot_image_start);


	if(hdr->dt_size != 0) {

		/* add kernel offset */
		dt_image_offset += page_size;
		n = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
		dt_image_offset += n;

		/* add ramdisk offset */
		n = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
		dt_image_offset += n;

		/* add second offset */
		if(hdr->second_size != 0) {
			n = ROUND_TO_PAGE(hdr->second_size, page_mask);
			dt_image_offset += n;
		}

		/* offset now point to start of dt.img */
		table = boot_image_start + dt_image_offset;

		/* Restriction that the device tree entry table should be less than a page*/
		ASSERT(((table->num_entries * sizeof(struct dt_entry))+ DEV_TREE_HEADER_SIZE) < hdr->page_size);

		/* Validate the device tree table header */
		if((table->magic != DEV_TREE_MAGIC) && (table->version != DEV_TREE_VERSION)) {
			dprintf(CRITICAL, "ERROR: Cannot validate Device Tree Table \n");
			return -1;
		}

		/* Calculate the offset of device tree within device tree table */
		if((dt_entry_ptr = get_device_tree_ptr(table)) == NULL){
			dprintf(CRITICAL, "ERROR: Getting device tree address failed\n");
			return -1;
		}

		/* Validate and Read device device tree in the "tags_add */
		if (check_aboot_addr_range_overlap(hdr->tags_addr, dt_entry_ptr->size))
		{
			dprintf(CRITICAL, "Device tree addresses overlap with aboot addresses.\n");
			return -1;
		}

		/* Read device device tree in the "tags_add */
		memmove((void*) hdr->tags_addr,
				boot_image_start + dt_image_offset +  dt_entry_ptr->offset,
				dt_entry_ptr->size);
	}

	/* Everything looks fine. Return success. */
	return 0;
}
#endif

void cmd_boot(const char *arg, void *data, unsigned sz)
{
	unsigned kernel_actual;
	unsigned ramdisk_actual;
	uint32_t image_actual;
	uint32_t dt_actual = 0;
	struct boot_img_hdr *hdr;
	char *ptr = ((char*) data);

	if (sz < sizeof(hdr)) {
		fastboot_fail("invalid bootimage header");
		return;
	}

	hdr = (struct boot_img_hdr *)data;

	/* ensure commandline is terminated */
	hdr->cmdline[BOOT_ARGS_SIZE-1] = 0;

	if(target_is_emmc_boot() && hdr->page_size) {
		page_size = hdr->page_size;
		page_mask = page_size - 1;
	}

	kernel_actual = ROUND_TO_PAGE(hdr->kernel_size, page_mask);
	ramdisk_actual = ROUND_TO_PAGE(hdr->ramdisk_size, page_mask);
#if DEVICE_TREE
	dt_actual = ROUND_TO_PAGE(hdr->dt_size, page_mask);
#endif

	image_actual = ADD_OF(page_size, kernel_actual);
	image_actual = ADD_OF(image_actual, ramdisk_actual);
	image_actual = ADD_OF(image_actual, dt_actual);

	/* sz should have atleast raw boot image */
	if (image_actual > sz) {
		fastboot_fail("incomplete bootimage");
		return;
	}

	/* Verify the boot image
	 * device & page_size are initialized in aboot_init
	 */
	if (target_use_signed_kernel() && (!device.is_unlocked))
		verify_signed_bootimg((uint32_t)data, image_actual);

	/* Get virtual addresses since the hdr saves physical addresses. */
	hdr->kernel_addr = VA(hdr->kernel_addr);
	hdr->ramdisk_addr = VA(hdr->ramdisk_addr);
	hdr->tags_addr = VA(hdr->tags_addr);

	/* Check if the addresses in the header are valid. */
	if (check_aboot_addr_range_overlap(hdr->kernel_addr, kernel_actual) ||
		check_aboot_addr_range_overlap(hdr->ramdisk_addr, ramdisk_actual))
	{
		dprintf(CRITICAL, "kernel/ramdisk addresses overlap with aboot addresses.\n");
		return;
	}

#if DEVICE_TREE
	/* find correct dtb and copy it to right location */
	ret = copy_dtb(data);

	dtb_copied = !ret ? 1 : 0;
#else
	if (check_aboot_addr_range_overlap(hdr->tags_addr, MAX_TAGS_SIZE))
	{
		dprintf(CRITICAL, "Tags addresses overlap with aboot addresses.\n");
		return;
	}
#endif

	/* Load ramdisk & kernel */
	memmove((void*) hdr->ramdisk_addr, ptr + page_size + kernel_actual, hdr->ramdisk_size);
	memmove((void*) hdr->kernel_addr, ptr + page_size, hdr->kernel_size);

#if DEVICE_TREE
	/*
	 * If dtb is not found look for appended DTB in the kernel.
	 * If appended dev tree is found, update the atags with
	 * memory address to the DTB appended location on RAM.
	 * Else update with the atags address in the kernel header
	 */
	if (!dtb_copied) {
		void *dtb;
		dtb = dev_tree_appended((void *)hdr->kernel_addr, (void *)hdr->tags_addr, hdr->kernel_size);
		if (!dtb) {
			fastboot_fail("dtb not found");
			return;
	}
#endif

#ifndef DEVICE_TREE
	if (check_aboot_addr_range_overlap(hdr->tags_addr, MAX_TAGS_SIZE))
	{
		dprintf(CRITICAL, "Tags addresses overlap with aboot addresses.\n");
		return;
	}
#endif

	check_boot_image();

	fastboot_okay("");
	udc_stop();

	memmove((void*) hdr->ramdisk_addr, ptr + page_size + kernel_actual, hdr->ramdisk_size);
	memmove((void*) hdr->kernel_addr, ptr + page_size, hdr->kernel_size);

	pu_reason |= RESTART_EVENT_NORMAL;

	boot_linux((void*) hdr->kernel_addr, (void*) hdr->tags_addr,
		   (const char*) hdr->cmdline, board_machtype(),
		   (void*) hdr->ramdisk_addr, hdr->ramdisk_size);
}

void cmd_erase_nand(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (flash_erase(ptn)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}

void cmd_erase_mmc(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	unsigned int out[512] = {0};
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);

	if(ptn == 0) {
		fastboot_fail("Partition table doesn't exist\n");
		return;
	}
	/* Simple inefficient version of erase. Just writing
       0 in first block */
	if (mmc_write(ptn , 512, (unsigned int *)out)) {
		fastboot_fail("failed to erase partition");
		return;
	}
	fastboot_okay("");
}

void cmd_erase(const char *arg, void *data, unsigned sz)
{
	if(target_is_emmc_boot())
		cmd_erase_mmc(arg, data, sz);

	else
		cmd_erase_nand(arg, data, sz);
}

void cmd_flash_mmc_img(const char *arg, void *data, unsigned sz)
{
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	if (!strcmp(arg, "partition-erase"))
	{
		dprintf(INFO, "Attempt to write partition image.\n");
		if (write_partition(sz, (unsigned char *) data, 1)) {
			fastboot_fail("failed to write partition");
			return;
		}
	}
	else if (!strcmp(arg, "partition"))
	{
		dprintf(INFO, "Attempt to write partition image w/o erasing.\n");
		if (write_partition(sz, (unsigned char *) data, 0)) {
			fastboot_fail("failed to write partition");
			return;
		}
	}
	else
	{
		index = partition_get_index(arg);
		ptn = partition_get_offset(index);
		if(ptn == 0) {
			fastboot_fail("partition table doesn't exist");
			return;
		}

		if (!strcmp(arg, "boot") || !strcmp(arg, "boot1")
				|| !strcmp(arg, "recovery")) {
			if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
				fastboot_fail("image is not a boot image");
				return;
			}
		}

		size = partition_get_size(index);
		if (ROUND_TO_PAGE(sz,511) > size) {
			fastboot_fail("size too large");
			return;
		}
		else if (mmc_write(ptn , sz, (unsigned int *)data)) {
			fastboot_fail("flash write failure");
			return;
		}
	}
	fastboot_okay("");
	return;
}

void cmd_flash_mmc_sparse_img(const char *arg, void *data, unsigned sz)
{
	unsigned int chunk;
	unsigned int chunk_data_sz;
	sparse_header_t *sparse_header;
	chunk_header_t *chunk_header;
	uint32_t total_blocks = 0;
	unsigned long long ptn = 0;
	unsigned long long size = 0;
	int index = INVALID_PTN;

	index = partition_get_index(arg);
	ptn = partition_get_offset(index);
	if(ptn == 0) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	size = partition_get_size(index);
	if (ROUND_TO_PAGE(sz,511) > size) {
		fastboot_fail("size too large");
		return;
	}

	/* Read and skip over sparse image header */
	sparse_header = (sparse_header_t *) data;
	data += sparse_header->file_hdr_sz;
	if(sparse_header->file_hdr_sz > sizeof(sparse_header_t))
	{
		/* Skip the remaining bytes in a header that is longer than
		 * we expected.
		 */
		data += (sparse_header->file_hdr_sz - sizeof(sparse_header_t));
	}

	dprintf (SPEW, "=== Sparse Image Header ===\n");
	dprintf (SPEW, "magic: 0x%x\n", sparse_header->magic);
	dprintf (SPEW, "major_version: 0x%x\n", sparse_header->major_version);
	dprintf (SPEW, "minor_version: 0x%x\n", sparse_header->minor_version);
	dprintf (SPEW, "file_hdr_sz: %d\n", sparse_header->file_hdr_sz);
	dprintf (SPEW, "chunk_hdr_sz: %d\n", sparse_header->chunk_hdr_sz);
	dprintf (SPEW, "blk_sz: %d\n", sparse_header->blk_sz);
	dprintf (SPEW, "total_blks: %d\n", sparse_header->total_blks);
	dprintf (SPEW, "total_chunks: %d\n", sparse_header->total_chunks);

	/* Start processing chunks */
	for (chunk=0; chunk<sparse_header->total_chunks; chunk++)
	{
		/* Make sure the total image size does not exceed the partition size */
		if(((uint64_t)total_blocks * (uint64_t)sparse_header->blk_sz) >= size) {
			fastboot_fail("size too large");
			return;
		}
		/* Read and skip over chunk header */
		chunk_header = (chunk_header_t *) data;
		data += sizeof(chunk_header_t);

		dprintf (SPEW, "=== Chunk Header ===\n");
		dprintf (SPEW, "chunk_type: 0x%x\n", chunk_header->chunk_type);
		dprintf (SPEW, "chunk_data_sz: 0x%x\n", chunk_header->chunk_sz);
		dprintf (SPEW, "total_size: 0x%x\n", chunk_header->total_sz);

		if(sparse_header->chunk_hdr_sz > sizeof(chunk_header_t))
		{
			/* Skip the remaining bytes in a header that is longer than
			 * we expected.
			 */
			data += (sparse_header->chunk_hdr_sz - sizeof(chunk_header_t));
		}

		chunk_data_sz = sparse_header->blk_sz * chunk_header->chunk_sz;

		/* Make sure multiplication does not overflow uint32 size */
		if (sparse_header->blk_sz && (chunk_header->chunk_sz != chunk_data_sz / sparse_header->blk_sz))
		{
			fastboot_fail("Bogus size sparse and chunk header");
			return;
		}

		/* Make sure that the chunk size calculated from sparse image does not
		 * exceed partition size
		 */
		if ((uint64_t)total_blocks * (uint64_t)sparse_header->blk_sz + chunk_data_sz > size)
		{
			fastboot_fail("Chunk data size exceeds partition size");
			return;
		}

		switch (chunk_header->chunk_type)
		{
			case CHUNK_TYPE_RAW:
			if(chunk_header->total_sz != (sparse_header->chunk_hdr_sz +
											chunk_data_sz))
			{
				fastboot_fail("Bogus chunk size for chunk type Raw");
				return;
			}

			if(mmc_write(ptn + ((uint64_t)total_blocks*sparse_header->blk_sz),
						chunk_data_sz,
						(unsigned int*)data))
			{
				fastboot_fail("flash write failure");
				return;
			}
			if(total_blocks > (UINT_MAX - chunk_header->chunk_sz)) {
				fastboot_fail("Bogus size for RAW chunk type");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			case CHUNK_TYPE_DONT_CARE:
			if(total_blocks > (UINT_MAX - chunk_header->chunk_sz)) {
				fastboot_fail("bogus size for chunk DONT CARE type");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			break;

			case CHUNK_TYPE_CRC:
			if(chunk_header->total_sz != sparse_header->chunk_hdr_sz)
			{
				fastboot_fail("Bogus chunk size for chunk type Dont Care");
				return;
			}
			if(total_blocks > (UINT_MAX - chunk_header->chunk_sz)) {
				fastboot_fail("bogus size for chunk CRC type");
				return;
			}
			total_blocks += chunk_header->chunk_sz;
			data += chunk_data_sz;
			break;

			default:
			fastboot_fail("Unknown chunk type");
			return;
		}
	}

	dprintf(INFO, "Wrote %d blocks, expected to write %d blocks\n",
					total_blocks, sparse_header->total_blks);

	if(total_blocks != sparse_header->total_blks)
	{
		fastboot_fail("sparse image write failure");
	}

	fastboot_okay("");
	return;
}

void cmd_flash_mmc(const char *arg, void *data, unsigned sz)
{
	sparse_header_t *sparse_header;
	/* 8 Byte Magic + 2048 Byte xml + Encrypted Data */
	unsigned int *magic_number = (unsigned int *) data;
	int ret=0;

	if (magic_number[0] == DECRYPT_MAGIC_0 &&
		magic_number[1] == DECRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = decrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Invalid secure image\n");
			return;
		}
	}
	else if (magic_number[0] == ENCRYPT_MAGIC_0 &&
			 magic_number[1] == ENCRYPT_MAGIC_1)
	{
#ifdef SSD_ENABLE
		ret = encrypt_scm((uint32 **) &data, &sz);
#endif
		if (ret != 0) {
			dprintf(CRITICAL, "ERROR: Encryption Failure\n");
			return;
		}
	}

	sparse_header = (sparse_header_t *) data;
	if (sparse_header->magic != SPARSE_HEADER_MAGIC)
		cmd_flash_mmc_img(arg, data, sz);
	else
		cmd_flash_mmc_sparse_img(arg, data, sz);
	return;
}

void cmd_flash_nand(const char *arg, void *data, unsigned sz)
{
	struct ptentry *ptn;
	struct ptable *ptable;
	unsigned extra = 0;

	ptable = flash_get_ptable();
	if (ptable == NULL) {
		fastboot_fail("partition table doesn't exist");
		return;
	}

	ptn = ptable_find(ptable, arg);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	if (!strcmp(ptn->name, "boot") || !strcmp(ptn->name, "recovery")) {
		if (memcmp((void *)data, BOOT_MAGIC, BOOT_MAGIC_SIZE)) {
			fastboot_fail("image is not a boot image");
			return;
		}
	}

	if (!strcmp(ptn->name, "system")
		|| !strcmp(ptn->name, "userdata")
		|| !strcmp(ptn->name, "persist")
		|| !strcmp(ptn->name, "recoveryfs")) {
		if (flash_ecc_bch_enabled())
			/* Spare data bytes for 8 bit ECC increased by 4 */
			extra = ((page_size >> 9) * 20);
		else
			extra = ((page_size >> 9) * 16);
	} else
		sz = ROUND_TO_PAGE(sz, page_mask);

	dprintf(INFO, "writing %d bytes to '%s'\n", sz, ptn->name);
	if (flash_write(ptn, extra, data, sz)) {
		fastboot_fail("flash write failure");
		return;
	}
	dprintf(INFO, "partition '%s' updated\n", ptn->name);
	fastboot_okay("");
}

void cmd_flash(const char *arg, void *data, unsigned sz)
{
	if(target_is_emmc_boot())
		cmd_flash_mmc(arg, data, sz);
	else
		cmd_flash_nand(arg, data, sz);
}


void cmd_continue(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	udc_stop();
	if (target_is_emmc_boot())
	{
		boot_linux_from_mmc();
	}
	else
	{
		boot_linux_from_flash();
	}
}

void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(NORMAL_MODE);
}

void cmd_reboot_bootloader(const char *arg, void *data, unsigned sz)
{
	dprintf(INFO, "rebooting the device\n");
	fastboot_okay("");
	reboot_device(FASTBOOT_MODE);
}

void cmd_oem_unlock(const char *arg, void *data, unsigned sz)
{
	if(!device.is_unlocked)
	{
		device.is_unlocked = 1;
		write_device_info(&device);
	}
	fastboot_okay("");
}

void cmd_oem_devinfo(const char *arg, void *data, unsigned sz)
{
	char response[64];
	snprintf(response, 64, "\tDevice tampered: %s", (device.is_tampered ? "true" : "false"));
	fastboot_info(response);
	snprintf(response, 64, "\tDevice unlocked: %s", (device.is_unlocked ? "true" : "false"));
	fastboot_info(response);
	fastboot_okay("");
}

void splash_screen ()
{
	struct ptentry *ptn;
	struct ptable *ptable;
	struct fbcon_config *fb_display = NULL;

	if (!target_is_emmc_boot())
	{
		ptable = flash_get_ptable();
		if (ptable == NULL) {
			dprintf(CRITICAL, "ERROR: Partition table not found\n");
			return;
		}

		ptn = ptable_find(ptable, "splash");
		if (ptn == NULL) {
			dprintf(CRITICAL, "ERROR: No splash partition found\n");
		} else {
			fb_display = fbcon_display();
			if (fb_display) {
				if (flash_read(ptn, 0, fb_display->base,
					(fb_display->width * fb_display->height * fb_display->bpp/8))) {
					fbcon_clear();
					dprintf(CRITICAL, "ERROR: Cannot read splash image\n");
				}
			}
		}
	}
}

static char machtype_buf[16] = {0};
static char soc_id_buf[16] = {0};
static char soc_version_buf[16] = {0};
static char pmic_model_buf[16] = "unknown";
static char pmic_die_version_buf[16] = "unknown";
#define SOCINFO_VERSION_MAJOR(ver) ((ver & 0xffff0000) >> 16)
#define SOCINFO_VERSION_MINOR(ver) (ver & 0x0000ffff)

/* register commands and variables for fastboot */
void aboot_fastboot_register_commands(void)
{
	int i;
	struct fastboot_cmd_desc cmd_list[] = {
						/* By default the enabled list is empty. */
						{"", NULL},
						/* move commands enclosed within the below ifndef to here
						if they need to be enabled in user build*/
#ifndef DISABLE_FASTBOOT_CMDS
						/* Register the following commands only for non-user builds */
						{"flash:", cmd_flash},
						{"erase:", cmd_erase},
						{"boot", cmd_boot},
						{"continue", cmd_continue},
						{"reboot", cmd_reboot},
						{"reboot-bootloader", cmd_reboot_bootloader},
						{"oem unlock", cmd_oem_unlock},
						{"oem device-info", cmd_oem_devinfo},
#endif

						};

	int fastboot_cmds_count = sizeof(cmd_list)/sizeof(cmd_list[0]);
        for (i = 1; i < fastboot_cmds_count; i++)
                fastboot_register(cmd_list[i].name,cmd_list[i].cb);

	fastboot_publish("product", TARGET(BOARD));
	fastboot_publish("kernel", "lk");
	fastboot_publish("serialno", sn_buf);
	fastboot_publish("machtype", machtype_buf);
	fastboot_publish("soc-id", soc_id_buf);
	fastboot_publish("soc-version", soc_version_buf);
	fastboot_publish("pmic-model", pmic_model_buf);
	fastboot_publish("pmic-die-version", pmic_die_version_buf);

}
void aboot_init(const struct app_descriptor *app)
{
	unsigned usb_init = 0;
	unsigned sz = 0;
	uint32_t ret = 0;

	/* Setup page size information for nand/emmc reads */
	if (target_is_emmc_boot())
	{
		page_size = 2048;
		page_mask = page_size - 1;
	}
	else
	{
		page_size = flash_page_size();
		page_mask = page_size - 1;
	}

	ASSERT((MEMBASE + MEMSIZE) > MEMBASE);

	if(target_use_signed_kernel())
	{
		read_device_info(&device);

	}

	target_serialno((unsigned char *) sn_buf);
	dprintf(SPEW,"serial number: %s\n",sn_buf);
	surf_udc_device.serialno = sn_buf;
	sprintf(machtype_buf, "%04x", board_target_id());
	dprintf(CRITICAL, "mach type: %s, 0x%x\n", machtype_buf, board_target_id());
	ret = board_soc_id();
	sprintf(soc_id_buf, "%d\n", ret);
	dprintf(CRITICAL, "soc id: %s, 0x%x\n", soc_id_buf, ret);
	ret = board_soc_version();
	sprintf(soc_version_buf, "%u.%u\n", SOCINFO_VERSION_MAJOR(ret), SOCINFO_VERSION_MINOR(ret));
	dprintf(CRITICAL, "soc version: %s, 0x%x\n", soc_version_buf, ret);
	ret = board_pmic_model();
	if (ret) {
		sprintf(pmic_model_buf, "%d\n", ret);
		dprintf(CRITICAL, "pmic type: %s, 0x%x\n", ret);
	}
	ret = board_pmic_die_version();
	if (ret) {
		sprintf(pmic_die_version_buf, "%d\n", ret);
		dprintf(CRITICAL, "pmic version: %s, 0x%x\n", ret);
	}

	/* Check if we should do something other than booting up */
	if ((keys_get_state(KEY_VOLUMEUP) != 0) && !(keys_get_state(KEY_VOLUMEDOWN) != 0)) {
		dprintf(INFO, "KEY_VOLUMEUP pressed\n");
		boot_into_recovery = 1;
	}
	if(!boot_into_recovery)
	{
		if (!(keys_get_state(KEY_VOLUMEUP) != 0) && (keys_get_state(KEY_VOLUMEDOWN) != 0)) {
			dprintf(INFO, "KEY_VOLUMEDOWN pressed\n");
			fastboot_reason = 1;
			goto fastboot;
		}
	}

	#if NO_KEYPAD_DRIVER
	if (fastboot_trigger())
		goto fastboot;
	#endif

	if (reboot_mode == RECOVERY_MODE) {
		boot_into_recovery = 1;
	} else if (reboot_mode == FASTBOOT_MODE) {
		fastboot_reason = 2;
		goto fastboot;
	} else if (reboot_mode == BOOT0_MODE) {
		dual_boot_sign = DUALBOOT_BOOT_FIRST;
	} else if (reboot_mode == BOOT1_MODE) {
		dual_boot_sign = DUALBOOT_BOOT_SECOND;
	}
	dprintf(INFO, "boot_into_recovery %d \n", boot_into_recovery);

	if (target_is_emmc_boot())
	{
		if(emmc_recovery_init())
			dprintf(ALWAYS,"error in emmc_recovery_init\n");
		if(target_use_signed_kernel())
		{
			if((device.is_unlocked) || (device.is_tampered))
			{
			#ifdef TZ_TAMPER_FUSE
				set_tamper_fuse_cmd();
			#endif
			#if USE_PCOM_SECBOOT
				set_tamper_flag(device.is_tampered);
			#endif
			}
		}
		boot_linux_from_mmc();
	}
	else
	{
		recovery_init();
#if USE_PCOM_SECBOOT
	if((device.is_unlocked) || (device.is_tampered))
		set_tamper_flag(device.is_tampered);
#endif
		boot_linux_from_flash();
	}
	fastboot_reason = 3;
	dprintf(CRITICAL, "ERROR: Could not do normal boot. Reverting "
		"to fastboot mode.\n");

fastboot:

	target_fastboot_init(fastboot_reason);

	if(!usb_init)
		udc_init(&surf_udc_device);

	aboot_fastboot_register_commands();
	partition_dump();
	sz = target_get_max_flash_size();
	fastboot_init(target_get_scratch_address(), sz);
	udc_start();
}

APP_START(aboot)
	.init = aboot_init,
APP_END

#if DEVICE_TREE
struct dt_entry * get_device_tree_ptr(struct dt_table *table)
{
	unsigned i;
	struct dt_entry *dt_entry_ptr;

	dt_entry_ptr = (char *)table + DEV_TREE_HEADER_SIZE ;

	for(i = 0; i < table->num_entries; i++)
	{
		if((dt_entry_ptr->platform_id == board_platform_id()) &&
		   (dt_entry_ptr->variant_id == board_hardware_id()) &&
		   (dt_entry_ptr->soc_rev == 0)){
				return dt_entry_ptr;
		}
		dt_entry_ptr++;
	}
	return NULL;
}

int update_device_tree(const void * fdt, char *cmdline,
					   void *ramdisk, unsigned ramdisk_size)
{
	int ret = 0;
	int offset;
	uint32_t *memory_reg;
	unsigned char *final_cmdline;
	uint32_t len;

	/* Check the device tree header */
	ret = fdt_check_header(fdt);
	if(ret)
	{
		dprintf(CRITICAL, "Invalid device tree header \n");
		return ret;
	}

	/* Get offset of the memory node */
	offset = fdt_path_offset(fdt,"/memory");

	memory_reg = target_dev_tree_mem(&len);

	/* Adding the memory values to the reg property */
	ret = fdt_setprop(fdt, offset, "reg", memory_reg, sizeof(uint32_t) * len * 2);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update memory node\n");
		return ret;
	}

	/* Get offset of the chosen node */
	offset = fdt_path_offset(fdt, "/chosen");

	/* Adding the cmdline to the chosen node */
	final_cmdline = update_cmdline(cmdline);
	ret = fdt_setprop_string(fdt, offset, "bootargs", final_cmdline);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [bootargs]\n");
		return ret;
	}

	/* Adding the initrd-start to the chosen node */
	ret = fdt_setprop_cell(fdt, offset, "linux,initrd-start", ramdisk);
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [linux,initrd-start]\n");
		return ret;
	}

	/* Adding the initrd-end to the chosen node */
	ret = fdt_setprop_cell(fdt, offset, "linux,initrd-end", (ramdisk + ramdisk_size));
	if(ret)
	{
		dprintf(CRITICAL, "ERROR: Cannot update chosen node [linux,initrd-end]\n");
		return ret;
	}

	fdt_pack(fdt);

	return ret;
}
#endif
