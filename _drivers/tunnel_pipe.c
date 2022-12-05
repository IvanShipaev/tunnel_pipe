#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>

/******************************************************************************
 * Macros PRINTF
 *****************************************************************************/
#define PRINTF(fmt, ...)	pr_info(fmt, ##__VA_ARGS__)
//#define PRINTF(fmt, ...) ((void)0)

/******************************************************************************
 * QUEUE
 *****************************************************************************/
typedef struct mqueue_t {
	u8 *buf;
	u32 bufsize;
	u32 len;
	u32 cnt_r;
	u32 cnt_w;
} mqueue_t;

static u32 mqueue_is_frame_ready(mqueue_t *queue)
{
	u32 len = 0;
	u32 cnt = queue->cnt_r;

	if (queue->len) {
		len |= ((u32)queue->buf[cnt]) <<  0; cnt = (cnt + 1) % queue->bufsize;
		len |= ((u32)queue->buf[cnt]) <<  8; cnt = (cnt + 1) % queue->bufsize;
		len |= ((u32)queue->buf[cnt]) << 16; cnt = (cnt + 1) % queue->bufsize;
		len |= ((u32)queue->buf[cnt]) << 24; cnt = (cnt + 1) % queue->bufsize;
	}
	return len;
}

static u32 mqueue_get_maxsize_data(mqueue_t *queue)
{
	return queue->bufsize - 4;
}

static u32 mqueue_get_free_space(mqueue_t *queue)
{
	return (queue->bufsize - queue->len) > 4 ? queue->bufsize - queue->len - 4 : 0;
}

static u32 mqueue_copy_to_user(char __user *buf, mqueue_t *queue)
{
	u32 lendata = mqueue_is_frame_ready(queue);
	u32 size = lendata;
	u32 cnt, len;

	if (!size)
		return 0;

	cnt = queue->cnt_r;
	cnt = (cnt + 4) % queue->bufsize;

	while(size) {
		len = min(size, queue->bufsize - cnt);
		if (copy_to_user(buf, queue->buf + cnt, len))
			return 0;
		size -= len;
		cnt = (cnt + len) % queue->bufsize;
	}
	queue->len -= lendata + 4;
	queue->cnt_r = cnt;

	return lendata;
}

static u32 mqueue_copy_from_user(mqueue_t *queue, const char __user *buf, size_t lendata)
{
	u32 size = lendata;
	u32 cnt, len;

	if (!size || size > mqueue_get_free_space(queue))
		return 0;

	cnt = queue->cnt_w;

	queue->buf[cnt] = (size >>  0) & 0xff; cnt = (cnt + 1) % queue->bufsize;
	queue->buf[cnt] = (size >>  8) & 0xff; cnt = (cnt + 1) % queue->bufsize;
	queue->buf[cnt] = (size >> 16) & 0xff; cnt = (cnt + 1) % queue->bufsize;
	queue->buf[cnt] = (size >> 24) & 0xff; cnt = (cnt + 1) % queue->bufsize;

	while(size) {
		len = min(size, queue->bufsize - cnt);
		if (copy_from_user(queue->buf + cnt, buf, len))
			return 0;
		size -= len;
		cnt = (cnt + len) % queue->bufsize;
	}
	queue->len += lendata + 4;
	queue->cnt_w = cnt;

	return lendata;
}

/******************************************************************************
 * TUNNEL
 *****************************************************************************/

typedef struct tunnel_t {
	struct cdev cdev;
	struct mutex mutex;
    wait_queue_head_t inq, outq;
    struct fasync_struct *async_queue;
    struct {
		struct file* file;
		mqueue_t queue;
	} id[2];
} tunnel_t;

static tunnel_t g_tunnel;
static dev_t g_first;
static struct class *g_class;
static ulong g_mqueue_len = 1024;
module_param(g_mqueue_len, ulong, 0440);

static int tunnel_open(struct inode * inode, struct file *file)
{
	int rc = 0;
	tunnel_t *tunnel = container_of(inode->i_cdev, tunnel_t, cdev);

	PRINTF("Tunnel: open()\n");

	if (mutex_lock_interruptible(&tunnel->mutex))
		return -ERESTARTSYS;

	if (!tunnel->id[0].file) {
		tunnel->id[0].file = file;
		file->private_data = tunnel;
	} else if (!tunnel->id[1].file) {
		tunnel->id[1].file = file;
		file->private_data = tunnel;
	} else
		rc = -EFAULT;

	mutex_unlock(&tunnel->mutex);

	return nonseekable_open(inode, file);
}

static int tunnel_fasync(int fd, struct file *file, int mode)
{
	tunnel_t *tunnel = file->private_data;

	return fasync_helper(fd, file, mode, &tunnel->async_queue);
}

static int tunnel_release(struct inode *inode, struct file *file)
{
	tunnel_t *tunnel = file->private_data;

	PRINTF("Tunnel: release()\n");

	tunnel_fasync(-1, file, 0);

	mutex_lock(&tunnel->mutex);

	if (tunnel->id[0].file == file)
		tunnel->id[0].file = NULL;
	else
		tunnel->id[1].file = NULL;

	mutex_unlock(&tunnel->mutex);

	return 0;
}

static ssize_t tunnel_read(struct file *file, char __user *buf, size_t len, loff_t *off)
{
	tunnel_t *tunnel = file->private_data;
	mqueue_t *queue;


	if (mutex_lock_interruptible(&tunnel->mutex))
		return -ERESTARTSYS;

	queue = tunnel->id[0].file != file ? &tunnel->id[0].queue : &tunnel->id[1].queue;

	while(!mqueue_is_frame_ready(queue)) {
		mutex_unlock(&tunnel->mutex);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(tunnel->inq, (mqueue_is_frame_ready(queue))))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&tunnel->mutex))
			return -ERESTARTSYS;
	}

	if (len < mqueue_is_frame_ready(queue)) {
		mutex_unlock(&tunnel->mutex);
		return -ETOOSMALL;
	}

	len = mqueue_copy_to_user(buf, queue);
	if (!len) {
		mutex_unlock(&tunnel->mutex);
		return -EFAULT;
	}

	*off += len;

	mutex_unlock(&tunnel->mutex);

	wake_up_interruptible(&tunnel->outq);

	return len;
}

static ssize_t tunnel_getwritespace(struct file *file, size_t len)
{
	tunnel_t *tunnel = file->private_data;
	mqueue_t *queue = tunnel->id[0].file == file ? &tunnel->id[0].queue : &tunnel->id[1].queue;


	while (mqueue_get_free_space(queue) < len) {
		DEFINE_WAIT(wait);

		mutex_unlock(&tunnel->mutex);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		prepare_to_wait(&tunnel->outq, &wait, TASK_INTERRUPTIBLE);
		if (mqueue_get_free_space(queue) < len)
			schedule();
		finish_wait(&tunnel->outq, &wait);

		if (signal_pending(current))
			return -ERESTARTSYS;
		if (mutex_lock_interruptible(&tunnel->mutex))
			return -ERESTARTSYS;
	}
	return 0;

}

static ssize_t tunnel_write(struct file *file, const char __user *buf, size_t len, loff_t *off)
{
	tunnel_t *tunnel = file->private_data;
	mqueue_t *queue = tunnel->id[0].file == file ? &tunnel->id[0].queue : &tunnel->id[1].queue;
	int rc;

	PRINTF("Tunnel: write()\n");

	if (mqueue_get_maxsize_data(queue) < len)
		return -ETOOSMALL;

	if (mutex_lock_interruptible(&tunnel->mutex))
		return -ERESTARTSYS;

	rc = tunnel_getwritespace(file, len);
	if (rc)
		return rc;

	if (!mqueue_copy_from_user(queue, buf, len)) {
		mutex_unlock(&tunnel->mutex);
		return -EFAULT;
	}

	*off += len;

	mutex_unlock(&tunnel->mutex);

	wake_up_interruptible(&tunnel->inq);

	if (tunnel->async_queue)
		kill_fasync(&tunnel->async_queue, SIGIO, POLL_IN);

	return len;
}

static struct file_operations gtunnel_fops = {
	.owner = THIS_MODULE,
	.open = tunnel_open,
	.release = tunnel_release,
	.read = tunnel_read,
	.write = tunnel_write,
	.llseek = no_llseek,
};

/******************************************************************************
 * Constructor
 * ***************************************************************************/
static int __init tunnel_init(void)
{
	PRINTF("Tunnel: init() whith param %lu\n", g_mqueue_len);

	mutex_init(&g_tunnel.mutex);
	init_waitqueue_head(&g_tunnel.inq);
	init_waitqueue_head(&g_tunnel.outq);

	g_tunnel.id[0].queue.bufsize = g_mqueue_len;
	g_tunnel.id[1].queue.bufsize = g_mqueue_len;

	g_tunnel.id[0].queue.buf = kmalloc(g_mqueue_len, GFP_KERNEL);
	g_tunnel.id[1].queue.buf = kmalloc(g_mqueue_len, GFP_KERNEL);

	if (g_tunnel.id[0].queue.buf == NULL ||
		g_tunnel.id[1].queue.buf == NULL)
	{
		goto _exit0;
	}


	if (alloc_chrdev_region(&g_first, 0, 1, "tunnel_pipe") < 0)
		goto _exit0;

	g_class = class_create(THIS_MODULE, "tunnel_pipe");
	if (!g_class)
	{
		unregister_chrdev_region(g_first, 1);
		goto _exit0;
	}

	if (device_create(g_class, NULL, g_first, NULL, "tunnel_pipe") == NULL)
	{
		class_destroy(g_class);
		unregister_chrdev_region(g_first, 1);
		goto _exit0;
	}

	cdev_init(&g_tunnel.cdev, &gtunnel_fops);
	if (0 != cdev_add(&g_tunnel.cdev, g_first, 1)) {
		class_destroy(g_class);
		unregister_chrdev_region(g_first, 1);
		goto _exit0;
	}

	return 0;

_exit0:

	kfree(g_tunnel.id[0].queue.buf);
	kfree(g_tunnel.id[1].queue.buf);

	return -1;
}

/******************************************************************************
 * Destructor
 * ***************************************************************************/
static void __exit tunnel_exit(void)
{
	PRINTF("Tunnel: exit()\n");

	cdev_del(&g_tunnel.cdev);
	device_destroy(g_class, g_first);
	class_destroy(g_class);
	unregister_chrdev_region(g_first, 1);
}

module_init(tunnel_init);
module_exit(tunnel_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
MODULE_AUTHOR("Ivan Shipaev, email: ivanshipaev84@gmail.com");
MODULE_DESCRIPTION("Tunnel PIPE driver");


