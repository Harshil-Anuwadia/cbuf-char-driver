/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_CBUF_H
#define _UAPI_CBUF_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct cbuf_stats {
	__u32 buf_size;
	__u32 occupancy;
	__u64 total_written;
	__u64 total_read;
	__u32 nr_readers;
	__u32 nr_writers;
};

#define CBUF_IOC_MAGIC	'C'

#define CBUF_IOC_FLUSH	_IO(CBUF_IOC_MAGIC, 0)
#define CBUF_IOC_STATS	_IOR(CBUF_IOC_MAGIC, 1, struct cbuf_stats)
#define CBUF_IOC_RESIZE	_IOW(CBUF_IOC_MAGIC, 2, __u32)

#endif /* _UAPI_CBUF_H */
