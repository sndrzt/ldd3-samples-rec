/* -*- C -*-
 * main.c -- the bare scull char module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: _main.c.in,v 1.21 2004/10/14 20:11:39 corbet Exp $
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/aio.h>
#include <asm/uaccess.h>
#include "scull.h"		/* local definitions */


int scull_major =   SCULL_MAJOR;
int scull_devs =    SCULL_DEVS;	/* number of bare scull devices */
int scull_qset =    SCULL_QSET;
int scull_order =   SCULL_ORDER;

module_param(scull_major, int, 0);
module_param(scull_devs, int, 0);
module_param(scull_qset, int, 0);
module_param(scull_order, int, 0);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

struct scull_dev *scull_devices; /* allocated in scull_init */

int scull_trim(struct scull_dev *dev);
void scull_cleanup(void);

static struct ldd_driver scull_driver = { /* Device model stuff */
	.version = "$Revision: 1.21 $",
	.module = THIS_MODULE,
	.driver = {
		.name = "scull",
	},
};



#ifdef SCULL_USE_PROC /* don't waste space if unused */
/*
 * The proc filesystem: function to read and entry
 */

void scull_proc_offset(char *buf, char **start, off_t *offset, int *len)
{
	if (*offset == 0)
		return;
	if (*offset >= *len) {
		/* Not there yet */
		*offset -= *len;
		*len = 0;
	} else {
		/* We're into the interesting stuff now */
		*start = buf + *offset;
		*offset = 0;
	}
}

/* FIXME: Do we need this here??  It be ugly  */
int scull_read_procmem(char *buf, char **start, off_t offset,
                   int count, int *eof, void *data)
{
	int i, j, order, qset, len = 0;
	int limit = count - 80; /* Don't print more than this */
	struct scull_dev *d;

	*start = buf;
	for(i = 0; i < scull_devs; i++) {
		d = &scull_devices[i];
		if (down_interruptible (&d->sem))
			return -ERESTARTSYS;
		qset = d->qset;  /* retrieve the features of each device */
		order = d->order;
		len += sprintf(buf+len,"\nDevice %i: qset %i, order %i, sz %li\n",
				i, qset, order, (long)(d->size));
		for (; d; d = d->next) { /* scan the list */
			len += sprintf(buf+len,"  item at %p, qset at %p\n",d,d->data);
			scull_proc_offset (buf, start, &offset, &len);
			if (len > limit)
				goto out;
			if (d->data && !d->next) /* dump only the last item - save space */
				for (j = 0; j < qset; j++) {
					if (d->data[j])
						len += sprintf(buf+len,"    % 4i:%8p\n",j,d->data[j]);
					scull_proc_offset (buf, start, &offset, &len);
					if (len > limit)
						goto out;
				}
		}
	  out:
		up (&scull_devices[i].sem);
		if (len > limit)
			break;
	}
	*eof = 1;
	return len;
}

#endif /* SCULL_USE_PROC */

/*
 * Open and close
 */

int scull_open (struct inode *inode, struct file *filp)
{
	struct scull_dev *dev; /* device information */

	/*  Find the device */
	dev = container_of(inode->i_cdev, struct scull_dev, cdev);

    	/* now trim to 0 the length of the device if open was write-only */
	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible (&dev->sem))
			return -ERESTARTSYS;
		scull_trim(dev); /* ignore errors */
		up (&dev->sem);
	}

	/* and use filp->private_data to point to the device data */
	filp->private_data = dev;

	return 0;          /* success */
}

int scull_release (struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * Follow the list 
 */
struct scull_dev *scull_follow(struct scull_dev *dev, int n)
{
	while (n--) {
		if (!dev->next) {
			dev->next = kmalloc(sizeof(struct scull_dev), GFP_KERNEL);
			memset(dev->next, 0, sizeof(struct scull_dev));
		}
		dev = dev->next;
		continue;
	}
	return dev;
}

/*
 * Data management: read and write
 */

ssize_t scull_read (struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data; /* the first listitem */
	struct scull_dev *dptr;
	int quantum = PAGE_SIZE << dev->order;
	int qset = dev->qset;
	int itemsize = quantum * qset; /* how many bytes in the listitem */
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;

	if (down_interruptible (&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos > dev->size) 
		goto nothing;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;
	/* find listitem, qset index, and offset in the quantum */
	item = ((long) *f_pos) / itemsize;
	rest = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

    	/* follow the list up to the right position (defined elsewhere) */
	dptr = scull_follow(dev, item);

	if (!dptr->data)
		goto nothing; /* don't fill holes */
	if (!dptr->data[s_pos])
		goto nothing;
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* read only up to the end of this quantum */

	if (copy_to_user (buf, dptr->data[s_pos]+q_pos, count)) {
		retval = -EFAULT;
		goto nothing;
	}
	up (&dev->sem);

	*f_pos += count;
	return count;

  nothing:
	up (&dev->sem);
	return retval;
}



ssize_t scull_write (struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_dev *dptr;
	int quantum = PAGE_SIZE << dev->order;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM; /* our most likely error */

	if (down_interruptible (&dev->sem))
		return -ERESTARTSYS;

	/* find listitem, qset index and offset in the quantum */
	item = ((long) *f_pos) / itemsize;
	rest = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	/* follow the list up to the right position */
	dptr = scull_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = (void *)__get_free_pages(GFP_KERNEL, dptr->order); /* allocation of a single quantum */
		if (!dptr->data[s_pos])
			goto nomem;
		memset(dptr->data[s_pos], 0, PAGE_SIZE << dptr->order);
	}
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* write only up to the end of this quantum */
	if (copy_from_user (dptr->data[s_pos]+q_pos, buf, count)) {
		retval = -EFAULT;
		goto nomem;
	}
	*f_pos += count;
 
    	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up (&dev->sem);
	return count;

  nomem:
	up (&dev->sem);
	return retval;
}

/*
 * The ioctl() implementation
 */

int scull_ioctl (struct inode *inode, struct file *filp,
                 unsigned int cmd, unsigned long arg)
{

	int err = 0, ret = 0, tmp;

	/* don't even decode wrong cmds: better returning  ENOTTY than EFAULT */
	if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) return -ENOTTY;
	if (_IOC_NR(cmd) > SCULL_IOC_MAXNR) return -ENOTTY;

	/*
	 * the type is a bitmask, and VERIFY_WRITE catches R/W
	 * transfers. Note that the type is user-oriented, while
	 * verify_area is kernel-oriented, so the concept of "read" and
	 * "write" is reversed
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	switch(cmd) {

	case SCULL_IOCRESET:
		scull_qset = SCULL_QSET;
		scull_order = SCULL_ORDER;
		break;

	case SCULL_IOCSORDER: /* Set: arg points to the value */
		ret = __get_user(scull_order, (int __user *) arg);
		break;

	case SCULL_IOCTORDER: /* Tell: arg is the value */
		scull_order = arg;
		break;

	case SCULL_IOCGORDER: /* Get: arg is pointer to result */
		ret = __put_user (scull_order, (int __user *) arg);
		break;

	case SCULL_IOCQORDER: /* Query: return it (it's positive) */
		return scull_order;

	case SCULL_IOCXORDER: /* eXchange: use arg as pointer */
		tmp = scull_order;
		ret = __get_user(scull_order, (int __user *) arg);
		if (ret == 0)
			ret = __put_user(tmp, (int __user *) arg);
		break;

	case SCULL_IOCHORDER: /* sHift: like Tell + Query */
		tmp = scull_order;
		scull_order = arg;
		return tmp;

	case SCULL_IOCSQSET:
		ret = __get_user(scull_qset, (int __user *) arg);
		break;

	case SCULL_IOCTQSET:
		scull_qset = arg;
		break;

	case SCULL_IOCGQSET:
		ret = __put_user(scull_qset, (int __user *)arg);
		break;

	case SCULL_IOCQQSET:
		return scull_qset;

	case SCULL_IOCXQSET:
		tmp = scull_qset;
		ret = __get_user(scull_qset, (int __user *)arg);
		if (ret == 0)
			ret = __put_user(tmp, (int __user *)arg);
		break;

	case SCULL_IOCHQSET:
		tmp = scull_qset;
		scull_qset = arg;
		return tmp;

	default:  /* redundant, as cmd was checked against MAXNR */
		return -ENOTTY;
	}

	return ret;
}

/*
 * The "extended" operations
 */

loff_t scull_llseek (struct file *filp, loff_t off, int whence)
{
	struct scull_dev *dev = filp->private_data;
	long newpos;

	switch(whence) {
	case 0: /* SEEK_SET */
		newpos = off;
		break;

	case 1: /* SEEK_CUR */
		newpos = filp->f_pos + off;
		break;

	case 2: /* SEEK_END */
		newpos = dev->size + off;
		break;

	default: /* can't happen */
		return -EINVAL;
	}
	if (newpos<0) return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}


/*
 * A simple asynchronous I/O implementation.
 */

struct async_work {
	struct kiocb *iocb;
	int result;
	struct work_struct work;
};

/*
 * "Complete" an asynchronous operation.
 */
static void scull_do_deferred_op(void *p)
{
	struct async_work *stuff = (struct async_work *) p;
	aio_complete(stuff->iocb, stuff->result, 0);
	kfree(stuff);
}


static int scull_defer_op(int write, struct kiocb *iocb, char __user *buf,
		size_t count, loff_t pos)
{
	struct async_work *stuff;
	int result;

	/* Copy now while we can access the buffer */
	if (write)
		result = scull_write(iocb->ki_filp, buf, count, &pos);
	else
		result = scull_read(iocb->ki_filp, buf, count, &pos);

	/* If this is a synchronous IOCB, we return our status now. */
	if (is_sync_kiocb(iocb))
		return result;

	/* Otherwise defer the completion for a few milliseconds. */
	stuff = kmalloc (sizeof (*stuff), GFP_KERNEL);
	if (stuff == NULL)
		return result; /* No memory, just complete now */
	stuff->iocb = iocb;
	stuff->result = result;
	INIT_WORK(&stuff->work, scull_do_deferred_op, stuff);
	schedule_delayed_work(&stuff->work, HZ/100);
	return -EIOCBQUEUED;
}


static ssize_t scull_aio_read(struct kiocb *iocb, char __user *buf, size_t count,
		loff_t pos)
{
	return scull_defer_op(0, iocb, buf, count, pos);
}

static ssize_t scull_aio_write(struct kiocb *iocb, const char __user *buf,
		size_t count, loff_t pos)
{
	return scull_defer_op(1, iocb, (char __user *) buf, count, pos);
}

extern int scull_mmap(struct file *filp, struct vm_area_struct *vma); /* Mmap *is* available, but confined in a different file */
/*
 * The fops
 */

struct file_operations scull_fops = {
	.owner =     THIS_MODULE,
	.llseek =    scull_llseek,
	.read =	     scull_read,
	.write =     scull_write,
	.ioctl =     scull_ioctl,
	.mmap =	     scull_mmap,
	.open =	     scull_open,
	.release =   scull_release,
	.aio_read =  scull_aio_read,
	.aio_write = scull_aio_write,
};

int scull_trim(struct scull_dev *dev)
{
	struct scull_dev *next, *dptr;
	int qset = dev->qset;   /* "dev" is not-null */
	int i;

	if (dev->vmas) /* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) { /* all the list items */
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					free_pages((unsigned long)(dptr->data[i]), dptr->order); /* This code frees a whole quantum-set */

			kfree(dptr->data);
			dptr->data=NULL;
		}
		next=dptr->next;
		if (dptr != dev) kfree(dptr); /* all of them but the first */
	}
	dev->size = 0;
	dev->qset = scull_qset;
	dev->order = scull_order;
	dev->next = NULL;
	return 0;
}


static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err, devno = MKDEV(scull_major, index);
    
	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

static ssize_t scull_show_dev(struct device *ddev, char *buf)
{
	struct scull_dev *dev = ddev->driver_data;

	return print_dev_t(buf, dev->cdev.dev);
}

static DEVICE_ATTR(dev, S_IRUGO, scull_show_dev, NULL);

static void scull_register_dev(struct scull_dev *dev, int index)
{
	sprintf(dev->devname, "scull%d", index);
	dev->ldev.name = dev->devname;
	dev->ldev.driver = &scull_driver;
	dev->ldev.dev.driver_data = dev;
	register_ldd_device(&dev->ldev);
	device_create_file(&dev->ldev.dev, &dev_attr_dev);
}


/*
 * Finally, the module stuff
 */

int scull_init(void)
{
	int result, i;
	dev_t dev = MKDEV(scull_major, 0);
	
	/*
	 * Register your major, and accept a dynamic number.
	 */
	if (scull_major)
		result = register_chrdev_region(dev, scull_devs, "scull");
	else {
		result = alloc_chrdev_region(&dev, 0, scull_devs, "scull");
		scull_major = MAJOR(dev);
	}
	if (result < 0)
		return result;

	register_ldd_driver(&scull_driver); /* Register with the driver core. */
	
	/* 
	 * allocate the devices -- we can't have them static, as the number
	 * can be specified at load time
	 */
	scull_devices = kmalloc(scull_devs*sizeof (struct scull_dev), GFP_KERNEL);
	if (!scull_devices) {
		result = -ENOMEM;
		goto fail_malloc;
	}
	memset(scull_devices, 0, scull_devs*sizeof (struct scull_dev));
	for (i = 0; i < scull_devs; i++) {
		scull_devices[i].order = scull_order;
		scull_devices[i].qset = scull_qset;
		sema_init (&scull_devices[i].sem, 1);
		scull_setup_cdev(scull_devices + i, i);
		scull_register_dev(scull_devices + i, i);
	}


#ifdef SCULL_USE_PROC /* only when available */
	create_proc_read_entry("scullmem", 0, NULL, scull_read_procmem, NULL);
#endif
	return 0; /* succeed */

  fail_malloc:
	unregister_chrdev_region(dev, scull_devs);
	return result;
}



void scull_cleanup(void)
{
	int i;

#ifdef SCULL_USE_PROC
	remove_proc_entry("scullmem", NULL);
#endif

	for (i = 0; i < scull_devs; i++) {
		unregister_ldd_device(&scull_devices[i].ldev);
		cdev_del(&scull_devices[i].cdev);
		scull_trim(scull_devices + i);
	}
	kfree(scull_devices);
	unregister_ldd_driver(&scull_driver);
	unregister_chrdev_region(MKDEV (scull_major, 0), scull_devs);
}


module_init(scull_init);
module_exit(scull_cleanup);
