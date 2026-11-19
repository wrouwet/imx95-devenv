// SPDX-License-Identifier: GPL-2.0
/*
 * m7shm: a memory window in the i.MX95 Cortex-M7 DTCM, shared with Linux.
 *
 * The window is a plain range of M7 TCM that the A55 reaches over the M7's
 * system-bus slave port (the M7 sees DTCM at 0x20000000, the A55 at
 * 0x20400000; see imx_rproc_att_imx95_m7). There is no protocol here: this
 * driver only makes the memory safely usable from Linux.
 *
 * Safety: the TCM is powered with the M7. When remoteproc stops the M7, an
 * A55 access to the window raises an SError and takes the kernel down. A
 * remoteproc subdevice tracks the M7's run state; while the M7 is down,
 * user mappings are zapped (accesses fault with SIGBUS), read/write return
 * ENODEV and m7shm_access_begin() returns NULL.
 *
 * Device tree:
 *	m7-shm@20420000 {
 *		compatible = "devenv,imx95-m7shm";
 *		reg = <0 0x20420000 0 0x20000>;		// A55 view
 *		devenv,m7-addr = <0x20020000>;		// M7 view
 *		devenv,rproc = <&cm7>;
 *	};
 */

#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "m7shm.h"
#include "m7shm_uapi.h"

struct m7shm {
	struct device *dev;
	phys_addr_t phys;
	size_t size;
	u32 m7_addr;
	void __iomem *base;

	struct rproc *rproc;
	struct rproc_subdev subdev;

	/*
	 * Protects @online. Held for read during every access to the window,
	 * so going offline (write) waits for accesses in flight.
	 */
	struct rw_semaphore lock;
	bool online;
	struct blocking_notifier_head notifier;

	struct miscdevice misc;
	/*
	 * Mapping that all opens of /dev/m7shm share, so going offline can zap
	 * every user mapping. Taken from the device node's inode on first
	 * open; devtmpfs keeps that inode while the device exists.
	 */
	struct mutex mapping_lock;
	struct address_space *mapping;
};

/* There is one window; m7shm_get() hands it to kernel clients. */
static DEFINE_MUTEX(m7shm_global_lock);
static struct m7shm *m7shm_global;

static void m7shm_set_online(struct m7shm *shm, bool online)
{
	if (shm->online == online)
		return;

	if (online) {
		down_write(&shm->lock);
		shm->online = true;
		up_write(&shm->lock);
		dev_info(shm->dev, "online: %zu KiB at %pa (M7 0x%08x)\n",
			 shm->size / 1024, &shm->phys, shm->m7_addr);
		blocking_notifier_call_chain(&shm->notifier, M7SHM_ONLINE, shm);
		return;
	}

	blocking_notifier_call_chain(&shm->notifier, M7SHM_OFFLINE, shm);
	down_write(&shm->lock);
	shm->online = false;
	up_write(&shm->lock);
	mutex_lock(&shm->mapping_lock);
	if (shm->mapping)
		unmap_mapping_range(shm->mapping, 0, 0, 1);
	mutex_unlock(&shm->mapping_lock);
	dev_info(shm->dev, "offline\n");
}

/* remoteproc subdevice: start runs after the M7 started, stop before it stops */
static int m7shm_rproc_start(struct rproc_subdev *subdev)
{
	m7shm_set_online(container_of(subdev, struct m7shm, subdev), true);
	return 0;
}

static void m7shm_rproc_stop(struct rproc_subdev *subdev, bool crashed)
{
	m7shm_set_online(container_of(subdev, struct m7shm, subdev), false);
}

/* --- in-kernel API --- */

struct m7shm *m7shm_get(void)
{
	struct m7shm *shm;

	mutex_lock(&m7shm_global_lock);
	shm = m7shm_global;
	if (shm)
		get_device(shm->dev);
	mutex_unlock(&m7shm_global_lock);
	return shm ?: ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(m7shm_get);

void m7shm_put(struct m7shm *shm)
{
	put_device(shm->dev);
}
EXPORT_SYMBOL_GPL(m7shm_put);

size_t m7shm_size(struct m7shm *shm)
{
	return shm->size;
}
EXPORT_SYMBOL_GPL(m7shm_size);

phys_addr_t m7shm_phys(struct m7shm *shm)
{
	return shm->phys;
}
EXPORT_SYMBOL_GPL(m7shm_phys);

u32 m7shm_m7_addr(struct m7shm *shm)
{
	return shm->m7_addr;
}
EXPORT_SYMBOL_GPL(m7shm_m7_addr);

void __iomem *m7shm_access_begin(struct m7shm *shm)
{
	down_read(&shm->lock);
	if (shm->online)
		return shm->base;
	up_read(&shm->lock);
	return NULL;
}
EXPORT_SYMBOL_GPL(m7shm_access_begin);

void m7shm_access_end(struct m7shm *shm)
{
	up_read(&shm->lock);
}
EXPORT_SYMBOL_GPL(m7shm_access_end);

int m7shm_register_notifier(struct m7shm *shm, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&shm->notifier, nb);
}
EXPORT_SYMBOL_GPL(m7shm_register_notifier);

int m7shm_unregister_notifier(struct m7shm *shm, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&shm->notifier, nb);
}
EXPORT_SYMBOL_GPL(m7shm_unregister_notifier);

/* --- /dev/m7shm --- */

static int m7shm_open(struct inode *inode, struct file *file)
{
	struct m7shm *shm = container_of(file->private_data, struct m7shm, misc);

	mutex_lock(&shm->mapping_lock);
	if (!shm->mapping)
		shm->mapping = inode->i_mapping;
	file->f_mapping = shm->mapping;
	mutex_unlock(&shm->mapping_lock);
	file->private_data = shm;
	return 0;
}

static ssize_t m7shm_copy(struct m7shm *shm, char __user *ubuf, size_t len,
			  loff_t *ppos, bool write)
{
	loff_t pos = *ppos;
	size_t done = 0;
	ssize_t ret = 0;
	void *bounce;

	if (pos < 0)
		return -EINVAL;
	if (pos >= shm->size)
		return write ? -ENOSPC : 0;
	len = min_t(size_t, len, shm->size - pos);

	bounce = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	while (done < len) {
		size_t n = min_t(size_t, len - done, PAGE_SIZE);

		if (write && copy_from_user(bounce, ubuf + done, n)) {
			ret = -EFAULT;
			break;
		}
		down_read(&shm->lock);
		if (!shm->online) {
			up_read(&shm->lock);
			ret = -ENODEV;
			break;
		}
		if (write)
			memcpy_toio(shm->base + pos + done, bounce, n);
		else
			memcpy_fromio(bounce, shm->base + pos + done, n);
		up_read(&shm->lock);
		if (!write && copy_to_user(ubuf + done, bounce, n)) {
			ret = -EFAULT;
			break;
		}
		done += n;
	}

	kfree(bounce);
	if (!done)
		return ret;
	*ppos = pos + done;
	return done;
}

static ssize_t m7shm_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	return m7shm_copy(file->private_data, buf, len, ppos, false);
}

static ssize_t m7shm_write(struct file *file, const char __user *buf, size_t len,
			   loff_t *ppos)
{
	return m7shm_copy(file->private_data, (char __user *)buf, len, ppos, true);
}

static loff_t m7shm_llseek(struct file *file, loff_t offset, int whence)
{
	struct m7shm *shm = file->private_data;

	return fixed_size_llseek(file, offset, whence, shm->size);
}

static vm_fault_t m7shm_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct m7shm *shm = vma->vm_private_data;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	if (vmf->pgoff >= shm->size >> PAGE_SHIFT)
		return VM_FAULT_SIGBUS;

	/*
	 * Insert under the read lock: going offline takes the write lock
	 * before zapping, so no page can be inserted after the zap.
	 */
	down_read(&shm->lock);
	if (shm->online)
		ret = vmf_insert_pfn(vma, vmf->address, PHYS_PFN(shm->phys) + vmf->pgoff);
	up_read(&shm->lock);
	return ret;
}

static const struct vm_operations_struct m7shm_vm_ops = {
	.fault = m7shm_vm_fault,
};

static int m7shm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct m7shm *shm = file->private_data;
	unsigned long pages = vma_pages(vma);

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_pgoff >= shm->size >> PAGE_SHIFT ||
	    pages > (shm->size >> PAGE_SHIFT) - vma->vm_pgoff)
		return -EINVAL;

	/* Pages are inserted on fault, so a stopped M7 gives SIGBUS, not SError */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	vma->vm_private_data = shm;
	vma->vm_ops = &m7shm_vm_ops;
	return 0;
}

static long m7shm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct m7shm *shm = file->private_data;
	struct m7shm_info info = {
		.host_addr = shm->phys,
		.m7_addr = shm->m7_addr,
		.size = shm->size,
	};

	if (cmd != M7SHM_IOC_INFO)
		return -ENOTTY;

	down_read(&shm->lock);
	info.online = shm->online;
	up_read(&shm->lock);
	return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static const struct file_operations m7shm_fops = {
	.owner = THIS_MODULE,
	.open = m7shm_open,
	.read = m7shm_read,
	.write = m7shm_write,
	.llseek = m7shm_llseek,
	.mmap = m7shm_mmap,
	.unlocked_ioctl = m7shm_ioctl,
};

/* --- sysfs --- */

static ssize_t online_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m7shm *shm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(shm->online));
}
static DEVICE_ATTR_RO(online);

static ssize_t m7_addr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m7shm *shm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", shm->m7_addr);
}
static DEVICE_ATTR_RO(m7_addr);

static ssize_t size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct m7shm *shm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%zu\n", shm->size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *m7shm_attrs[] = {
	&dev_attr_online.attr,
	&dev_attr_m7_addr.attr,
	&dev_attr_size.attr,
	NULL
};
ATTRIBUTE_GROUPS(m7shm);

/* --- platform driver --- */

static int m7shm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	struct m7shm *shm;
	phandle ph;
	int ret;

	shm = devm_kzalloc(dev, sizeof(*shm), GFP_KERNEL);
	if (!shm)
		return -ENOMEM;
	shm->dev = dev;
	init_rwsem(&shm->lock);
	mutex_init(&shm->mapping_lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&shm->notifier);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no reg\n");
	shm->phys = res->start;
	shm->size = resource_size(res);
	if (!PAGE_ALIGNED(shm->phys) || !PAGE_ALIGNED(shm->size))
		return dev_err_probe(dev, -EINVAL, "window must be page aligned\n");
	if (of_property_read_u32(np, "devenv,m7-addr", &shm->m7_addr))
		return dev_err_probe(dev, -EINVAL, "no devenv,m7-addr\n");

	/* Same attributes imx_rproc uses for TCM; no request_mem_region, imx_rproc maps TCM too */
	shm->base = devm_ioremap_wc(dev, shm->phys, shm->size);
	if (!shm->base)
		return -ENOMEM;

	if (of_property_read_u32(np, "devenv,rproc", &ph))
		return dev_err_probe(dev, -EINVAL, "no devenv,rproc\n");
	shm->rproc = rproc_get_by_phandle(ph);
	if (!shm->rproc)
		return dev_err_probe(dev, -EPROBE_DEFER, "remoteproc not ready\n");

	platform_set_drvdata(pdev, shm);

	shm->subdev.start = m7shm_rproc_start;
	shm->subdev.stop = m7shm_rproc_stop;
	mutex_lock(&shm->rproc->lock);
	rproc_add_subdev(shm->rproc, &shm->subdev);
	/* The subdev only sees future starts; pick up an M7 that already runs */
	if (shm->rproc->state == RPROC_RUNNING || shm->rproc->state == RPROC_ATTACHED)
		m7shm_set_online(shm, true);
	mutex_unlock(&shm->rproc->lock);

	shm->misc.minor = MISC_DYNAMIC_MINOR;
	shm->misc.name = "m7shm";
	shm->misc.fops = &m7shm_fops;
	shm->misc.parent = dev;
	ret = misc_register(&shm->misc);
	if (ret)
		goto err_subdev;

	mutex_lock(&m7shm_global_lock);
	m7shm_global = shm;
	mutex_unlock(&m7shm_global_lock);

	if (!shm->online)
		dev_info(dev, "%zu KiB at %pa (M7 0x%08x), waiting for the M7\n",
			 shm->size / 1024, &shm->phys, shm->m7_addr);
	return 0;

err_subdev:
	mutex_lock(&shm->rproc->lock);
	rproc_remove_subdev(shm->rproc, &shm->subdev);
	mutex_unlock(&shm->rproc->lock);
	rproc_put(shm->rproc);
	return ret;
}

static void m7shm_remove(struct platform_device *pdev)
{
	struct m7shm *shm = platform_get_drvdata(pdev);

	mutex_lock(&m7shm_global_lock);
	m7shm_global = NULL;
	mutex_unlock(&m7shm_global_lock);

	misc_deregister(&shm->misc);

	mutex_lock(&shm->rproc->lock);
	m7shm_set_online(shm, false);
	rproc_remove_subdev(shm->rproc, &shm->subdev);
	mutex_unlock(&shm->rproc->lock);
	rproc_put(shm->rproc);
}

static const struct of_device_id m7shm_of_match[] = {
	{ .compatible = "devenv,imx95-m7shm" },
	{ }
};
MODULE_DEVICE_TABLE(of, m7shm_of_match);

static struct platform_driver m7shm_driver = {
	.probe = m7shm_probe,
	.remove = m7shm_remove,
	.driver = {
		.name = "m7shm",
		.of_match_table = m7shm_of_match,
		.dev_groups = m7shm_groups,
	},
};
module_platform_driver(m7shm_driver);

MODULE_DESCRIPTION("i.MX95 Cortex-M7 DTCM memory window");
MODULE_LICENSE("GPL");
