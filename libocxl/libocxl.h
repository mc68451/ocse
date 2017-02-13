/*
 * Copyright 2014,2015 International Business Machines
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _LIBOCXL_H
#define _LIBOCXL_H

//#include <linux/types.h>
#include <misc/ocxl.h>
#include <stdbool.h>
#include <stddef.h>  // for size_t
#include <stdint.h>
#include <stdio.h>  // for off_t

#ifdef __cplusplus
extern "C" {
#endif

#define OCXL_KERNEL_API_VERSION 1

#define OCXL_SYSFS_CLASS "/sys/class/ocxl"
#define OCXL_DEV_DIR "/dev/ocxl"

/*
 * Opaque types
 */
struct ocxl_adapter_h;
struct ocxl_afu_h;
struct ocxl_ioctl_start_work;

/*
 * Adapter Enumeration
 *
 * Repeatedly call ocxl_adapter_next() (or use the ocxl_for_each_adapter macro)
 * to enumerate the available OCXL adapters.
 *
 * ocxl_adapter_next() will implicitly free used buffers if it is called on the
 * last adapter, or ocxl_adapter_free() can be called explicitly.
 */
struct ocxl_adapter_h *ocxl_adapter_next(struct ocxl_adapter_h *adapter);
char *ocxl_adapter_dev_name(struct ocxl_adapter_h *adapter);
void ocxl_adapter_free(struct ocxl_adapter_h *adapter);
#define ocxl_for_each_adapter(adapter) \
	for (adapter = ocxl_adapter_next(NULL); adapter; adapter = ocxl_adapter_next(adapter))

/*
 * AFU Enumeration
 *
 * Repeatedly call ocxl_adapter_afu_next() (or use the
 * ocxl_for_each_adapter_afu macro) to enumerate AFUs on a specific OCXL
 * adapter, or use ocxl_afu_next() or ocxl_for_each_afu to enumerate AFUs over
 * all OCXL adapters in the system.
 *
 * For instance, if you just want to find any AFU attached to the system but
 * don't particularly care which one, just do:
 * struct ocxl_afu_h *afu_h = ocxl_afu_next(NULL);
 *
 * ocxl_[adapter]_afu_next() will implicitly free used buffers if it is called
 * on the last AFU, or ocxl_afu_free() can be called explicitly.
 */
struct ocxl_afu_h *ocxl_adapter_afu_next(struct ocxl_adapter_h *adapter,
				       struct ocxl_afu_h *afu);
struct ocxl_afu_h *ocxl_afu_next(struct ocxl_afu_h *afu);
char *ocxl_afu_dev_name(struct ocxl_afu_h *afu);
#define ocxl_for_each_adapter_afu(adapter, afu) \
	for (afu = ocxl_adapter_afu_next(adapter, NULL); afu; afu = ocxl_adapter_afu_next(adapter, afu))
#define ocxl_for_each_afu(afu) \
	for (afu = ocxl_afu_next(NULL); afu; afu = ocxl_afu_next(afu))

enum ocxl_views {
	OCXL_VIEW_DEDICATED = 0,
	OCXL_VIEW_MASTER,
	OCXL_VIEW_SLAVE
};

/*
 * Open AFU - either by path, by AFU being enumerated, or tie into an AFU file
 * descriptor that has already been opened. The AFU file descriptor will be
 * closed by ocxl_afu_free() regardless of how it was opened.
 */
struct ocxl_afu_h *ocxl_afu_open_dev(char *path);
struct ocxl_afu_h *ocxl_afu_open_h(struct ocxl_afu_h *afu, enum ocxl_views view);
//struct ocxl_afu_h * ocxl_afu_fd_to_h(int fd);
void ocxl_afu_free(struct ocxl_afu_h *afu);
int ocxl_afu_opened(struct ocxl_afu_h *afu);

/*
 * Attach AFU context to this process
 */
struct ocxl_ioctl_start_work *ocxl_work_alloc(void);
int ocxl_work_free(struct ocxl_ioctl_start_work *work);
int ocxl_work_get_amr(struct ocxl_ioctl_start_work *work, __u64 *valp);
int ocxl_work_get_num_irqs(struct ocxl_ioctl_start_work *work, __s16 *valp);
int ocxl_work_get_wed(struct ocxl_ioctl_start_work *work, __u64 *valp);
int ocxl_work_set_amr(struct ocxl_ioctl_start_work *work, __u64 amr);
int ocxl_work_set_num_irqs(struct ocxl_ioctl_start_work *work, __s16 num_irqs);
int ocxl_work_set_wed(struct ocxl_ioctl_start_work *work, __u64 wed);

int ocxl_afu_attach(struct ocxl_afu_h *afu, uint64_t wed);
int ocxl_afu_attach_work(struct ocxl_afu_h *afu,
			struct ocxl_ioctl_start_work *work);

/* Deprecated interface */
int ocxl_afu_attach_full(struct ocxl_afu_h *afu, uint64_t wed,
			uint16_t num_interrupts, uint64_t amr);

/*
 * Get AFU process element
 */
int ocxl_afu_get_process_element(struct ocxl_afu_h *afu);

/*
 * Returns the file descriptor for the open AFU to use with event loops.
 * Returns -1 if the AFU is not open.
 */
int ocxl_afu_fd(struct ocxl_afu_h *afu);

/*
 * sysfs helpers
 */

/*
 * NOTE: On success, this function automatically allocates the returned
 * buffer, which must be freed by the caller (much like asprintf).
 */
//int ocxl_afu_sysfs_pci(struct ocxl_afu_h *afu, char **pathp);

/* Flags for ocxl_get/set_mode and ocxl_get_modes_supported */
#define OCXL_MODE_DEDICATED   0x1
#define OCXL_MODE_DIRECTED    0x2
#define OCXL_MODE_TIME_SLICED 0x4

/* Values for ocxl_get/set_prefault_mode */
enum ocxl_prefault_mode {
	OCXL_PREFAULT_MODE_NONE = 0,
	OCXL_PREFAULT_MODE_WED,
	OCXL_PREFAULT_MODE_ALL,
};

/* Values for ocxl_get_image_loaded */
enum ocxl_image {
	OCXL_IMAGE_FACTORY = 0,
	OCXL_IMAGE_USER,
};

/*
 * Get/set attribute values.
 * Return 0 on success, -1 on error.
 */
int ocxl_get_api_version(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_api_version_compatible(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_irqs_max(struct ocxl_afu_h *afu, long *valp);
int ocxl_set_irqs_max(struct ocxl_afu_h *afu, long value);
int ocxl_get_irqs_min(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_mmio_size(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_mode(struct ocxl_afu_h *afu, long *valp);
int ocxl_set_mode(struct ocxl_afu_h *afu, long value);
int ocxl_get_modes_supported(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_prefault_mode(struct ocxl_afu_h *afu, enum ocxl_prefault_mode *valp);
int ocxl_set_prefault_mode(struct ocxl_afu_h *afu, enum ocxl_prefault_mode value);
//int ocxl_get_dev(struct ocxl_afu_h *afu, long *majorp, long *minorp);
int ocxl_get_pp_mmio_len(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_pp_mmio_off(struct ocxl_afu_h *afu, long *valp);
int ocxl_get_base_image(struct ocxl_adapter_h *adapter, long *valp);
int ocxl_get_caia_version(struct ocxl_adapter_h *adapter, long *majorp,
                       long *minorp);
int ocxl_get_image_loaded(struct ocxl_adapter_h *adapter, enum ocxl_image *valp);
int ocxl_get_psl_revision(struct ocxl_adapter_h *adapter, long *valp);

/*
 * Events
 */
int ocxl_event_pending(struct ocxl_afu_h *afu);
int ocxl_read_event(struct ocxl_afu_h *afu, struct ocxl_event *event);
int ocxl_read_expected_event(struct ocxl_afu_h *afu, struct ocxl_event *event,
			    uint32_t type, uint16_t irq);

/*
 * fprint wrappers to print out OCXL events - useful for debugging.
 * ocxl_fprint_event will select the appropriate implementation based on the
 * event type and ocxl_fprint_unknown_event will print out a hex dump of the
 * raw event.
 */
//int ocxl_fprint_event(FILE *stream, struct ocxl_event *event);
//int ocxl_fprint_unknown_event(FILE *stream, struct ocxl_event *event);

/*
 * AFU MMIO functions
 *
 * The below assessors will byte swap based on what is passed to map.  Also a
 * full memory barrier 'sync' will proceed a write and follow a read.  More
 * relaxed assessors can be created using a pointer derived from ocxl_mmio_ptr().
 */
#define OCXL_MMIO_BIG_ENDIAN 0x1
#define OCXL_MMIO_LITTLE_ENDIAN 0x2
#define OCXL_MMIO_HOST_ENDIAN 0x3
#define OCXL_MMIO_ENDIAN_MASK 0x3
#define OCXL_MMIO_FLAGS 0x3
int ocxl_mmio_map(struct ocxl_afu_h *afu, uint32_t flags);
int ocxl_mmio_unmap(struct ocxl_afu_h *afu);

/* WARNING: Use of ocxl_mmio_ptr not supported for PSL Simulation Engine.
 * It is recommended that this function not be used but use the following MMIO
 * read/write functions instead. */
void *ocxl_mmio_ptr(struct ocxl_afu_h *afu);

int ocxl_mmio_write64(struct ocxl_afu_h *afu, uint64_t offset, uint64_t data);
int ocxl_mmio_read64(struct ocxl_afu_h *afu, uint64_t offset, uint64_t * data);
int ocxl_mmio_write32(struct ocxl_afu_h *afu, uint64_t offset, uint32_t data);
int ocxl_mmio_read32(struct ocxl_afu_h *afu, uint64_t offset, uint32_t * data);

/*
 * Calling this function will install the libocxl SIGBUS handler. This will
 * catch bad MMIO accesses (e.g. due to hardware failures) that would otherwise
 * terminate the program and make the above mmio functions return errors
 * instead.
 *
 * Call this once per process prior to any MMIO accesses.
 */
//use JK's temp fix for this
static inline int ocxl_mmio_install_sigbus_handler(void)
{
/* nothing to be done yet */
return 0;
}
int ocxl_get_cr_device(struct ocxl_afu_h *afu, long cr_num, long *valp);
int ocxl_get_cr_vendor(struct ocxl_afu_h *afu, long cr_num, long *valp);
int ocxl_get_cr_class(struct ocxl_afu_h *afu, long cr_num, long *valp);
int ocxl_errinfo_size(struct ocxl_afu_h *afu, size_t *valp);
int ocxl_errinfo_read(struct ocxl_afu_h *afu, void *dst, off_t off, size_t len);

#ifdef __cplusplus
}
#endif

#endif