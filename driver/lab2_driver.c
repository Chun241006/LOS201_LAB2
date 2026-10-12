/*
 * LAB-02: Simple Character Device Driver
 *
 * This driver demonstrates:
 *   - Character device registration
 *   - open(), read(), write(), release()
 *   - Kernel/User space data transfer
 *   - Mutex protection for shared data
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
/* Module information */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Embedded Linux Lab");
MODULE_DESCRIPTION("LAB-02: Simple Character Device Driver");
MODULE_VERSION("1.0");

/* Device configuration */
#define DEVICE_NAME  "lab2"
#define CLASS_NAME   "lab2_class"
#define BUFFER_SIZE  1024
#define DRIVER_MAJOR 240

/* Driver global variables */
static int major_number = DRIVER_MAJOR;

/*
 * Buffer used to store data written from userspace.
 * BUFFER_SIZE - 1 is used for data because the last byte
 * is reserved for the string terminating '\0'.
 */
static char device_buffer[BUFFER_SIZE];

static int buffer_len = 0;
static int open_count = 0;

static struct class *lab2_class = NULL;
static struct device *lab2_device = NULL;

/*
 * Mutex protects shared data such as:
 *   - device_buffer
 *   - buffer_len
 *   - open_count
 *
 * This prevents race conditions when multiple processes
 * access the driver at the same time.
 */
static DEFINE_MUTEX(lab2_mutex);
/* ===== PROCFS Implementation ===== */
static struct proc_dir_entry *proc_entry;

static int lab2_proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "LAB-02 Driver Statistics\n");
    seq_printf(m, "======================\n");
    seq_printf(m, "Driver name   : %s\n", DEVICE_NAME);
    seq_printf(m, "Major number  : %d\n", major_number);
    seq_printf(m, "Buffer size   : %d bytes\n", BUFFER_SIZE);
    seq_printf(m, "Data length   : %d bytes\n", buffer_len);
    seq_printf(m, "Open count    : %d\n", open_count);

    if (buffer_len > 0)
        seq_printf(m, "Last data     : [%s]\n", device_buffer);

    return 0;
}

static int lab2_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, lab2_proc_show, NULL);
}

static const struct proc_ops lab2_proc_fops = {
    .proc_open    = lab2_proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};
/* ===== SYSFS Implementation ===== */

static ssize_t buffer_len_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
    return sprintf(buf, "%d\n", buffer_len);
}

static ssize_t open_count_show(struct device *dev,
                               struct device_attribute *attr,
                               char *buf)
{
    return sprintf(buf, "%d\n", open_count);
}

static ssize_t last_data_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    if (buffer_len > 0)
        return sprintf(buf, "%s\n", device_buffer);

    return sprintf(buf, "(empty)\n");
}

static DEVICE_ATTR_RO(buffer_len);
static DEVICE_ATTR_RO(open_count);
static DEVICE_ATTR_RO(last_data);

static struct attribute *lab2_attrs[] = {
    &dev_attr_buffer_len.attr,
    &dev_attr_open_count.attr,
    &dev_attr_last_data.attr,
    NULL,
};

static struct attribute_group lab2_attr_group = {
    .attrs = lab2_attrs,
};
/*
 * ---------------------------------------------------------
 * lab2_open()
 * ---------------------------------------------------------
 *
 * Called when userspace opens:
 *
 *     /dev/lab2
 *
 * Example:
 *
 *     echo "Hello" > /dev/lab2
 *
 * The open count is protected by a mutex because multiple
 * processes may open the device simultaneously.
 */
static int lab2_open(struct inode *inode, struct file *file)
{
    mutex_lock(&lab2_mutex);

    open_count++;

    mutex_unlock(&lab2_mutex);

    pr_info("lab2_driver: device opened (count=%d)\n",
            open_count);

    return 0;
}


/*
 * ---------------------------------------------------------
 * lab2_release()
 * ---------------------------------------------------------
 *
 * Called when userspace closes the device.
 *
 * Example:
 *
 *     cat /dev/lab2
 *
 * After cat finishes, the file is released.
 */
static int lab2_release(struct inode *inode, struct file *file)
{
    pr_info("lab2_driver: device closed\n");

    return 0;
}


/*
 * ---------------------------------------------------------
 * lab2_read()
 * ---------------------------------------------------------
 *
 * Transfers data:
 *
 *     Kernel buffer -> Userspace buffer
 *
 * The userspace pointer cannot be accessed directly from
 * kernel space, therefore copy_to_user() must be used.
 *
 * read() returns:
 *   > 0 : number of bytes successfully read
 *     0 : end of data
 *    <0 : error
 */
static ssize_t lab2_read(struct file *file,
                         char __user *buf,
                         size_t len,
                         loff_t *offset)
{
    size_t bytes_to_read;

    /*
     * If the file offset has reached the end of the stored
     * data, return 0 to indicate EOF.
     */
    if (*offset >= buffer_len)
        return 0;

    /*
     * Do not read beyond the amount of valid data.
     */
    bytes_to_read = buffer_len - *offset;

    if (len > bytes_to_read)
        len = bytes_to_read;

    /*
     * copy_to_user():
     *
     * Kernel -> User
     *
     * It safely checks whether the userspace address is valid.
     */
    if (copy_to_user(buf,
                     device_buffer + *offset,
                     len)) {

        pr_err("lab2_driver: copy_to_user failed\n");

        return -EFAULT;
    }

    /*
     * Move file position forward so a subsequent read()
     * continues from the correct position.
     */
    *offset += len;

    pr_info("lab2_driver: sent %zu bytes to user\n", len);

    return len;
}


/*
 * ---------------------------------------------------------
 * lab2_write()
 * ---------------------------------------------------------
 *
 * Transfers data:
 *
 *     Userspace buffer -> Kernel buffer
 *
 * copy_from_user() must be used instead of memcpy()
 * because the source pointer belongs to userspace.
 *
 * The mutex protects the shared device buffer from
 * concurrent writes.
 */
static ssize_t lab2_write(struct file *file,
                          const char __user *buf,
                          size_t len,
                          loff_t *offset)
{
    /*
     * Reserve one byte for the terminating '\0'.
     */
    if (len > BUFFER_SIZE - 1) {

        pr_warn("lab2_driver: write too large, truncating\n");

        len = BUFFER_SIZE - 1;
    }

    /*
     * Lock shared data before modifying it.
     */
    mutex_lock(&lab2_mutex);

    /*
     * copy_from_user():
     *
     * User -> Kernel
     */
    if (copy_from_user(device_buffer, buf, len)) {

        mutex_unlock(&lab2_mutex);

        pr_err("lab2_driver: copy_from_user failed\n");

        return -EFAULT;
    }

    /*
     * Store the number of valid bytes.
     */
    buffer_len = len;

    /*
     * Add string terminator so the buffer can safely
     * be printed using %s.
     */
    device_buffer[buffer_len] = '\0';

    /*
     * Unlock shared data.
     */
    mutex_unlock(&lab2_mutex);

    pr_info("lab2_driver: received %zu bytes: [%s]\n",
            len, device_buffer);

    return len;
}


/*
 * ---------------------------------------------------------
 * File operations structure
 * ---------------------------------------------------------
 *
 * This structure connects userspace system calls with
 * the corresponding driver functions.
 *
 * open()    -> lab2_open()
 * read()    -> lab2_read()
 * write()   -> lab2_write()
 * close()   -> lab2_release()
 */
static const struct file_operations lab2_fops = {
    .owner   = THIS_MODULE,
    .open    = lab2_open,
    .release = lab2_release,
    .read    = lab2_read,
    .write   = lab2_write,
};


/*
 * ---------------------------------------------------------
 * lab2_init()
 * ---------------------------------------------------------
 *
 * Executed when the kernel loads the module.
 *
 * Responsibilities:
 *   1. Register character device
 *   2. Create device class
 *   3. Create device
 */
static int __init lab2_init(void)
{
    int ret;

    pr_info("lab2_driver: initializing module\n");

    /*
     * Register character device using fixed major number 240.
     */
    ret = register_chrdev(major_number,
                          DEVICE_NAME,
                          &lab2_fops);

    if (ret < 0) {

        pr_err("lab2_driver: register_chrdev failed: %d\n",
               ret);

        return ret;
    }

    /*
     * Create device class.
     *
     * This creates:
     *
     *     /sys/class/lab2_class/
     */
    lab2_class = class_create(THIS_MODULE, CLASS_NAME);

    if (IS_ERR(lab2_class)) {

        pr_err("lab2_driver: class_create failed\n");

        unregister_chrdev(major_number, DEVICE_NAME);

        return PTR_ERR(lab2_class);
    }

    /*
     * Create device.
     *
     * The kernel will expose the device through sysfs.
     */
    lab2_device = device_create(lab2_class,
                                NULL,
                                MKDEV(major_number, 0),
                                NULL,
                                DEVICE_NAME);

    if (IS_ERR(lab2_device)) {

        pr_err("lab2_driver: device_create failed\n");

        class_destroy(lab2_class);

        unregister_chrdev(major_number, DEVICE_NAME);

        return PTR_ERR(lab2_device);
    }

    pr_info("lab2_driver: loaded, major=%d\n",
            major_number);

    proc_entry = proc_create("lab2_info", 0444, NULL, &lab2_proc_fops);

    if (!proc_entry)
        pr_warn("lab2_driver: failed to create /proc/lab2_info\n");
    else
        pr_info("lab2_driver: /proc/lab2_info created\n");

    /* Create sysfs attributes */
    ret = sysfs_create_group(&lab2_device->kobj, &lab2_attr_group);
    if (ret)
        pr_warn("lab2_driver: sysfs_create_group failed\n");
    else
        pr_info("lab2_driver: sysfs attributes created\n");

    return 0;
}

/*
 * ---------------------------------------------------------
 * lab2_exit()
 * ---------------------------------------------------------
 *
 * Executed when the kernel unloads the module.
 *
 * It removes all resources allocated by lab2_init().
 */
static void __exit lab2_exit(void)
{
if (proc_entry)
    proc_remove(proc_entry);

  sysfs_remove_group(&lab2_device->kobj, &lab2_attr_group);

    /*
     * Remove device from sysfs.
     */
    device_destroy(lab2_class,
                   MKDEV(major_number, 0));

    /*
     * Remove device class.
     */
    class_destroy(lab2_class);

    /*
     * Unregister character device.
     */
    unregister_chrdev(major_number,
                     DEVICE_NAME);

    pr_info("lab2_driver: module unloaded\n");
}


/*
 * Tell the kernel which functions are the module
 * initialization and cleanup functions.
 */
module_init(lab2_init);
module_exit(lab2_exit);
