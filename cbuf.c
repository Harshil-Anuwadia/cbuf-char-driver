// SPDX-License-Identifier: GPL-2.0
/*
 * cbuf - Multi-instance circular buffer character device
 *
 * Exposes /dev/cbuf0../dev/cbufN as stream-oriented FIFOs backed by
 * per-device circular buffers.  Supports blocking and non-blocking I/O,
 * poll/epoll, fasync, ioctl for stats/flush/resize, and sysfs attributes.
 *
 * Copyright (C) 2026 Harshil Anuwadia
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "cbuf.h"

#define CBUF_MAX_DEVICES	16
#define CBUF_DEFAULT_SIZE	4096
#define CBUF_MIN_SIZE		64
#define CBUF_MAX_SIZE		(1 << 20)

static unsigned int num_devices = 4;
module_param(num_devices, uint, 0444);
MODULE_PARM_DESC(num_devices, "number of devices (default: 4, max: 16)");

static unsigned int buf_size = CBUF_DEFAULT_SIZE;
module_param(buf_size, uint, 0444);
MODULE_PARM_DESC(buf_size, "initial buffer size in bytes (default: 4096)");

struct cbuf_device {
	unsigned char		*buf;
	size_t			size;
	unsigned int		head;
	unsigned int		tail;
	unsigned int		count;

	struct mutex		lock;
	wait_queue_head_t	rd_wait;
	wait_queue_head_t	wr_wait;
	struct fasync_struct	*async_queue;

	struct cdev		cdev;
	struct device		*dev;
	int			minor;

	u64			total_written;
	u64			total_read;
	unsigned int		nr_readers;
	unsigned int		nr_writers;
};

static dev_t cbuf_devt;
static struct class *cbuf_class;
static struct cbuf_device *cbuf_devices;

/* ---- file operations --------------------------------------------------- */

static int cbuf_open(struct inode *inode, struct file *file)
{
	struct cbuf_device *cbd;

	cbd = container_of(inode->i_cdev, struct cbuf_device, cdev);
	file->private_data = cbd;

	mutex_lock(&cbd->lock);
	if (file->f_mode & FMODE_READ)
		cbd->nr_readers++;
	if (file->f_mode & FMODE_WRITE)
		cbd->nr_writers++;
	mutex_unlock(&cbd->lock);

	return nonseekable_open(inode, file);
}

static int cbuf_release(struct inode *inode, struct file *file)
{
	struct cbuf_device *cbd = file->private_data;

	fasync_helper(-1, file, 0, &cbd->async_queue);

	mutex_lock(&cbd->lock);
	if (file->f_mode & FMODE_READ)
		cbd->nr_readers--;
	if (file->f_mode & FMODE_WRITE)
		cbd->nr_writers--;
	mutex_unlock(&cbd->lock);

	return 0;
}

static ssize_t cbuf_read(struct file *file, char __user *ubuf,
			 size_t len, loff_t *off)
{
	struct cbuf_device *cbd = file->private_data;
	size_t to_read, first, second;

	if (mutex_lock_interruptible(&cbd->lock))
		return -ERESTARTSYS;

	while (!cbd->count) {
		mutex_unlock(&cbd->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(cbd->rd_wait, cbd->count))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&cbd->lock))
			return -ERESTARTSYS;
	}

	to_read = min(len, (size_t)cbd->count);
	first = min(to_read, cbd->size - cbd->tail);
	second = to_read - first;

	if (copy_to_user(ubuf, cbd->buf + cbd->tail, first)) {
		mutex_unlock(&cbd->lock);
		return -EFAULT;
	}

	if (second && copy_to_user(ubuf + first, cbd->buf, second)) {
		mutex_unlock(&cbd->lock);
		return -EFAULT;
	}

	cbd->tail = (cbd->tail + to_read) % cbd->size;
	cbd->count -= to_read;
	cbd->total_read += to_read;

	mutex_unlock(&cbd->lock);
	wake_up_interruptible(&cbd->wr_wait);
	kill_fasync(&cbd->async_queue, SIGIO, POLL_OUT);

	return to_read;
}

static ssize_t cbuf_write(struct file *file, const char __user *ubuf,
			  size_t len, loff_t *off)
{
	struct cbuf_device *cbd = file->private_data;
	size_t space, to_write, first, second;

	if (mutex_lock_interruptible(&cbd->lock))
		return -ERESTARTSYS;

	while (cbd->count == cbd->size) {
		mutex_unlock(&cbd->lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(cbd->wr_wait,
					     cbd->count < cbd->size))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&cbd->lock))
			return -ERESTARTSYS;
	}

	space = cbd->size - cbd->count;
	to_write = min(len, space);
	first = min(to_write, cbd->size - cbd->head);
	second = to_write - first;

	if (copy_from_user(cbd->buf + cbd->head, ubuf, first)) {
		mutex_unlock(&cbd->lock);
		return -EFAULT;
	}

	if (second && copy_from_user(cbd->buf, ubuf + first, second)) {
		mutex_unlock(&cbd->lock);
		return -EFAULT;
	}

	cbd->head = (cbd->head + to_write) % cbd->size;
	cbd->count += to_write;
	cbd->total_written += to_write;

	mutex_unlock(&cbd->lock);
	wake_up_interruptible(&cbd->rd_wait);
	kill_fasync(&cbd->async_queue, SIGIO, POLL_IN);

	return to_write;
}

static __poll_t cbuf_poll(struct file *file, poll_table *wait)
{
	struct cbuf_device *cbd = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &cbd->rd_wait, wait);
	poll_wait(file, &cbd->wr_wait, wait);

	mutex_lock(&cbd->lock);
	if (cbd->count)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (cbd->count < cbd->size)
		mask |= EPOLLOUT | EPOLLWRNORM;
	mutex_unlock(&cbd->lock);

	return mask;
}

static int cbuf_fasync(int fd, struct file *file, int on)
{
	struct cbuf_device *cbd = file->private_data;

	return fasync_helper(fd, file, on, &cbd->async_queue);
}

/* ---- ioctl helpers ----------------------------------------------------- */

static int cbuf_flush(struct cbuf_device *cbd)
{
	mutex_lock(&cbd->lock);
	cbd->head = 0;
	cbd->tail = 0;
	cbd->count = 0;
	mutex_unlock(&cbd->lock);

	wake_up_interruptible(&cbd->wr_wait);
	return 0;
}

static int cbuf_get_stats(struct cbuf_device *cbd,
			  struct cbuf_stats __user *ustat)
{
	struct cbuf_stats st;

	mutex_lock(&cbd->lock);
	st.buf_size	= cbd->size;
	st.occupancy	= cbd->count;
	st.total_written = cbd->total_written;
	st.total_read	= cbd->total_read;
	st.nr_readers	= cbd->nr_readers;
	st.nr_writers	= cbd->nr_writers;
	mutex_unlock(&cbd->lock);

	if (copy_to_user(ustat, &st, sizeof(st)))
		return -EFAULT;

	return 0;
}

static int cbuf_do_resize(struct cbuf_device *cbd, unsigned int new_size)
{
	unsigned char *new_buf, *old_buf;
	unsigned int first, count;

	if (new_size < CBUF_MIN_SIZE || new_size > CBUF_MAX_SIZE)
		return -EINVAL;

	new_size = roundup_pow_of_two(new_size);

	new_buf = kvzalloc(new_size, GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;

	mutex_lock(&cbd->lock);

	/* reject if current data would be truncated */
	if (cbd->count > new_size) {
		mutex_unlock(&cbd->lock);
		kvfree(new_buf);
		return -EBUSY;
	}

	count = cbd->count;
	if (count) {
		first = min(count, (unsigned int)(cbd->size - cbd->tail));
		memcpy(new_buf, cbd->buf + cbd->tail, first);
		if (count > first)
			memcpy(new_buf + first, cbd->buf, count - first);
	}

	old_buf = cbd->buf;
	cbd->buf = new_buf;
	cbd->size = new_size;
	cbd->head = count;
	cbd->tail = 0;

	mutex_unlock(&cbd->lock);
	kvfree(old_buf);

	wake_up_interruptible(&cbd->wr_wait);
	return 0;
}

static long cbuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cbuf_device *cbd = file->private_data;
	__u32 val;

	switch (cmd) {
	case CBUF_IOC_FLUSH:
		return cbuf_flush(cbd);

	case CBUF_IOC_STATS:
		return cbuf_get_stats(cbd, (struct cbuf_stats __user *)arg);

	case CBUF_IOC_RESIZE:
		if (get_user(val, (__u32 __user *)arg))
			return -EFAULT;
		return cbuf_do_resize(cbd, val);
	}

	return -ENOTTY;
}

static const struct file_operations cbuf_fops = {
	.owner		= THIS_MODULE,
	.open		= cbuf_open,
	.release	= cbuf_release,
	.read		= cbuf_read,
	.write		= cbuf_write,
	.poll		= cbuf_poll,
	.unlocked_ioctl	= cbuf_ioctl,
	.compat_ioctl	= cbuf_ioctl,
	.fasync		= cbuf_fasync,
	.llseek		= noop_llseek,
};

/* ---- sysfs ------------------------------------------------------------- */

static ssize_t buf_size_show(struct device *dev,
			     struct device_attribute *attr, char *page)
{
	struct cbuf_device *cbd = dev_get_drvdata(dev);

	return sysfs_emit(page, "%zu\n", cbd->size);
}
static DEVICE_ATTR_RO(buf_size);

static ssize_t occupancy_show(struct device *dev,
			      struct device_attribute *attr, char *page)
{
	struct cbuf_device *cbd = dev_get_drvdata(dev);
	unsigned int val;

	mutex_lock(&cbd->lock);
	val = cbd->count;
	mutex_unlock(&cbd->lock);

	return sysfs_emit(page, "%u\n", val);
}
static DEVICE_ATTR_RO(occupancy);

static ssize_t total_read_show(struct device *dev,
			       struct device_attribute *attr, char *page)
{
	struct cbuf_device *cbd = dev_get_drvdata(dev);

	return sysfs_emit(page, "%llu\n", cbd->total_read);
}
static DEVICE_ATTR_RO(total_read);

static ssize_t total_written_show(struct device *dev,
				  struct device_attribute *attr, char *page)
{
	struct cbuf_device *cbd = dev_get_drvdata(dev);

	return sysfs_emit(page, "%llu\n", cbd->total_written);
}
static DEVICE_ATTR_RO(total_written);

static struct attribute *cbuf_dev_attrs[] = {
	&dev_attr_buf_size.attr,
	&dev_attr_occupancy.attr,
	&dev_attr_total_read.attr,
	&dev_attr_total_written.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cbuf_dev);

/* ---- device lifecycle -------------------------------------------------- */

static int cbuf_setup_device(struct cbuf_device *cbd, int minor)
{
	int err;
	dev_t devno = MKDEV(MAJOR(cbuf_devt), minor);

	cbd->buf = kvzalloc(buf_size, GFP_KERNEL);
	if (!cbd->buf)
		return -ENOMEM;

	cbd->size = buf_size;
	cbd->minor = minor;
	mutex_init(&cbd->lock);
	init_waitqueue_head(&cbd->rd_wait);
	init_waitqueue_head(&cbd->wr_wait);

	cdev_init(&cbd->cdev, &cbuf_fops);
	cbd->cdev.owner = THIS_MODULE;

	err = cdev_add(&cbd->cdev, devno, 1);
	if (err)
		goto err_buf;

	cbd->dev = device_create(cbuf_class, NULL, devno, cbd, "cbuf%d", minor);
	if (IS_ERR(cbd->dev)) {
		err = PTR_ERR(cbd->dev);
		goto err_cdev;
	}

	return 0;

err_cdev:
	cdev_del(&cbd->cdev);
err_buf:
	kvfree(cbd->buf);
	return err;
}

static void cbuf_teardown_device(struct cbuf_device *cbd)
{
	device_destroy(cbuf_class, MKDEV(MAJOR(cbuf_devt), cbd->minor));
	cdev_del(&cbd->cdev);
	kvfree(cbd->buf);
}

/* ---- module init/exit -------------------------------------------------- */

static int __init cbuf_init(void)
{
	int err, i;

	if (!num_devices || num_devices > CBUF_MAX_DEVICES) {
		pr_err("num_devices=%u out of range (1-%u)\n",
		       num_devices, CBUF_MAX_DEVICES);
		return -EINVAL;
	}

	if (buf_size < CBUF_MIN_SIZE || buf_size > CBUF_MAX_SIZE) {
		pr_err("buf_size=%u out of range (%u-%u)\n",
		       buf_size, CBUF_MIN_SIZE, CBUF_MAX_SIZE);
		return -EINVAL;
	}
	buf_size = roundup_pow_of_two(buf_size);

	err = alloc_chrdev_region(&cbuf_devt, 0, num_devices, "cbuf");
	if (err)
		return err;

	cbuf_class = class_create("cbuf");
	if (IS_ERR(cbuf_class)) {
		err = PTR_ERR(cbuf_class);
		goto err_region;
	}
	cbuf_class->dev_groups = cbuf_dev_groups;

	cbuf_devices = kcalloc(num_devices, sizeof(*cbuf_devices), GFP_KERNEL);
	if (!cbuf_devices) {
		err = -ENOMEM;
		goto err_class;
	}

	for (i = 0; i < num_devices; i++) {
		err = cbuf_setup_device(&cbuf_devices[i], i);
		if (err)
			goto err_devices;
	}

	pr_info("registered %u devices (buf_size=%u)\n", num_devices, buf_size);
	return 0;

err_devices:
	while (i--)
		cbuf_teardown_device(&cbuf_devices[i]);
	kfree(cbuf_devices);
err_class:
	class_destroy(cbuf_class);
err_region:
	unregister_chrdev_region(cbuf_devt, num_devices);
	return err;
}

static void __exit cbuf_exit(void)
{
	unsigned int i;

	for (i = 0; i < num_devices; i++)
		cbuf_teardown_device(&cbuf_devices[i]);

	kfree(cbuf_devices);
	class_destroy(cbuf_class);
	unregister_chrdev_region(cbuf_devt, num_devices);
}

module_init(cbuf_init);
module_exit(cbuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harshil Anuwadia");
MODULE_DESCRIPTION("Multi-instance circular buffer character device");
