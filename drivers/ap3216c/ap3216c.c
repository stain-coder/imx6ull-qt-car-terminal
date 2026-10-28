#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/errno.h>
#include "ap3216creg.h"

#define AP3216C_NAME "ap3216c"

struct ap3216c_dev {
    struct i2c_client *client;
    struct cdev *cdev;
    struct class *class;
    struct device *device;
    dev_t devid;
    bool present;
    bool opened;
};

/* 静态状态不会因 I2C unbind 被释放，旧 fd 的 read 可安全返回 ENODEV。 */
static struct ap3216c_dev apdev;
static DEFINE_MUTEX(ap_lock);

/* 下列寄存器访问函数由持有 ap_lock 的调用者使用。 */
static int ap3216c_read_reg(struct i2c_client *client, u8 reg, u8 *value)
{
    struct i2c_msg msgs[2] = {
        { .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
        { .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = value }
    };
    int ret = i2c_transfer(client->adapter, msgs, 2);

    if (ret == 2)
        return 0;
    return ret < 0 ? ret : -EIO;
}

static int ap3216c_write_reg(struct i2c_client *client, u8 reg, u8 value)
{
    u8 bytes[2] = { reg, value };
    struct i2c_msg msg = {
        .addr = client->addr, .flags = 0,
        .len = sizeof(bytes), .buf = bytes
    };
    int ret = i2c_transfer(client->adapter, &msg, 1);

    if (ret == 1)
        return 0;
    return ret < 0 ? ret : -EIO;
}

static int ap3216c_hw_init(struct i2c_client *client)
{
    int ret;
    u8 mode;

    ret = ap3216c_write_reg(client, AP3216C_SYSTEMCONG, 0x04);
    if (ret)
        return ret;
    msleep(50); /* 沿用原例程的复位等待，使用可调度睡眠。 */
    ret = ap3216c_write_reg(client, AP3216C_SYSTEMCONG, 0x03);
    if (ret)
        return ret;
    msleep(150); /* 默认 ALS+PS+IR 周期约 112.5 ms，等待首次转换。 */
    ret = ap3216c_read_reg(client, AP3216C_SYSTEMCONG, &mode);
    if (ret)
        return ret;
    if ((mode & 0x07) != 0x03)
        return -EIO;
    /* 模式回读仅验证通信及配置，不是唯一芯片 ID 检测。 */
    return 0;
}

static int ap3216c_sample(struct i2c_client *client, u16 data[3])
{
    u8 b[6];
    int i, ret;

    /* 保留原逐寄存器读法，不假设芯片支持任意连续地址 burst。 */
    for (i = 0; i < ARRAY_SIZE(b); i++) {
        ret = ap3216c_read_reg(client, AP3216C_IRDATALOW + i, &b[i]);
        if (ret)
            return ret;
    }
    /* 旧 6 字节 ABI 没有有效位字段：整帧无效时返回错误，不伪造零值。 */
    if ((b[0] & 0x80) || (b[4] & 0x40))
        return -EOVERFLOW;

    data[0] = ((u16)b[1] << 2) | (b[0] & 0x03);
    data[1] = ((u16)b[3] << 8) | b[2];
    data[2] = ((u16)(b[5] & 0x3f) << 4) | (b[4] & 0x0f);
    return 0;
}

static int ap3216c_open(struct inode *inode, struct file *file)
{
    int ret;

    ret = mutex_lock_interruptible(&ap_lock);
    if (ret)
        return ret;
    /* cdev 比较拒绝移除/重新绑定期间对旧 cdev 的迟到 open。 */
    if (!apdev.present || inode->i_cdev != apdev.cdev) {
        ret = -ENODEV;
        goto out;
    }
    if (apdev.opened) {
        ret = -EBUSY;
        goto out;
    }
    if (file->f_mode & FMODE_WRITE) {
        ret = -EACCES;
        goto out;
    }
    ret = nonseekable_open(inode, file);
    if (!ret) {
        apdev.opened = true;
        file->private_data = &apdev;
    }
out:
    mutex_unlock(&ap_lock);
    return ret;
}

static ssize_t ap3216c_read(struct file *file, char __user *buf,
                            size_t count, loff_t *pos)
{
    struct ap3216c_dev *dev = file->private_data;
    u16 data[3];
    int ret;

    if (!count)
        return 0;
    if (count < sizeof(data))
        return -EINVAL;
    ret = mutex_lock_interruptible(&ap_lock);
    if (ret)
        return ret;
    if (!dev->present || !dev->client)
        ret = -ENODEV;
    else
        ret = ap3216c_sample(dev->client, data);
    mutex_unlock(&ap_lock);
    if (ret)
        return ret;
    if (copy_to_user(buf, data, sizeof(data)))
        return -EFAULT;
    return sizeof(data);
}

static int ap3216c_release(struct inode *inode, struct file *file)
{
    mutex_lock(&ap_lock);
    apdev.opened = false;
    file->private_data = NULL;
    mutex_unlock(&ap_lock);
    return 0;
}

static const struct file_operations ap3216c_fops = {
    .owner = THIS_MODULE,
    .open = ap3216c_open,
    .read = ap3216c_read,
    .release = ap3216c_release,
    .llseek = no_llseek,
};

static int ap3216c_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    int ret;

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
        return -EOPNOTSUPP;
    if (client->flags & I2C_CLIENT_TEN)
        return -EINVAL;
    mutex_lock(&ap_lock);
    /* 只支持一颗芯片；旧 fd 未关闭时不允许新绑定复用其状态。 */
    if (apdev.present || apdev.opened) {
        ret = -EBUSY;
        goto out;
    }
    ret = ap3216c_hw_init(client);
    if (ret) {
        dev_err(&client->dev, "initialization failed: %d\n", ret);
        goto power_down;
    }
    ret = alloc_chrdev_region(&apdev.devid, 0, 1, AP3216C_NAME);
    if (ret)
        goto power_down;
    apdev.class = class_create(THIS_MODULE, AP3216C_NAME);
    if (IS_ERR(apdev.class)) {
        ret = PTR_ERR(apdev.class);
        apdev.class = NULL;
        goto unregister_region;
    }
    /* 独立分配 cdev，让其引用计数管理迟到 open/旧 inode 的生命周期。 */
    apdev.cdev = cdev_alloc();
    if (!apdev.cdev) {
        ret = -ENOMEM;
        goto destroy_class;
    }
    apdev.cdev->owner = THIS_MODULE;
    apdev.cdev->ops = &ap3216c_fops;
    ret = cdev_add(apdev.cdev, apdev.devid, 1);
    if (ret) {
        kobject_put(&apdev.cdev->kobj);
        apdev.cdev = NULL;
        goto destroy_class;
    }
    apdev.device = device_create(apdev.class, &client->dev, apdev.devid,
                                 NULL, AP3216C_NAME);
    if (IS_ERR(apdev.device)) {
        ret = PTR_ERR(apdev.device);
        apdev.device = NULL;
        goto delete_cdev;
    }
    apdev.client = client;
    apdev.present = true;
    i2c_set_clientdata(client, &apdev);
    dev_info(&client->dev,
             "ready: /dev/ap3216c, ABI=u16[IR,ALS,PS], 6 bytes\n");
    mutex_unlock(&ap_lock);
    return 0;

delete_cdev:
    cdev_del(apdev.cdev);
    apdev.cdev = NULL;
destroy_class:
    class_destroy(apdev.class);
    apdev.class = NULL;
unregister_region:
    unregister_chrdev_region(apdev.devid, 1);
power_down:
    /* 保留原始错误码。尽力关机，失败不掩盖首个错误。 */
    ap3216c_write_reg(client, AP3216C_SYSTEMCONG, 0x00);
out:
    mutex_unlock(&ap_lock);
    return ret;
}

static int ap3216c_remove(struct i2c_client *client)
{
    int ret;

    mutex_lock(&ap_lock);
    apdev.present = false;
    device_destroy(apdev.class, apdev.devid);
    cdev_del(apdev.cdev);
    apdev.cdev = NULL;
    class_destroy(apdev.class);
    apdev.class = NULL;
    apdev.device = NULL;
    unregister_chrdev_region(apdev.devid, 1);
    ret = ap3216c_write_reg(client, AP3216C_SYSTEMCONG, 0x00);
    if (ret)
        dev_warn(&client->dev, "power down failed: %d\n", ret);
    apdev.client = NULL;
    i2c_set_clientdata(client, NULL);
    mutex_unlock(&ap_lock);
    return 0;
}

static const struct i2c_device_id ap3216c_ids[] = {
    { "alientek,ap3216c", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_ids);

static const struct of_device_id ap3216c_of_match[] = {
    { .compatible = "alientek,ap3216c" },
    { }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_driver = {
    .driver = {
        .name = AP3216C_NAME,
        .owner = THIS_MODULE,
        .of_match_table = ap3216c_of_match,
    },
    .probe = ap3216c_probe,
    .remove = ap3216c_remove,
    .id_table = ap3216c_ids,
};
module_i2c_driver(ap3216c_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("gongting");
MODULE_DESCRIPTION("AP3216C raw character interface for Linux 4.1.15");
