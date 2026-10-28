#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/errno.h>
#include "icm20608reg.h"

#define ICM20608_NAME "icm20608"

struct icm20608_dev {
    struct spi_device *spi;
    struct cdev *cdev;
    struct class *class;
    struct device *device;
    dev_t devid;
    bool present;
    bool opened;
};

/* 静态状态不会因 SPI unbind 被释放，旧 fd 的 read 可安全返回 ENODEV。 */
static struct icm20608_dev icmdev;
static DEFINE_MUTEX(icm_lock);

/* spi_write_then_read 使用内部可供 SPI 传输的缓冲区。
 * 一个消息内先发读命令，再收数据，期间保持片选有效。
 * 允许这里的 command 和调用者输出数组位于栈上，不把它们直接交给 DMA。
 */
static int icm20608_read_regs(struct spi_device *spi, u8 reg,
                             void *buffer, unsigned int len)
{
    u8 command = reg | 0x80; /* bit7=1：读寄存器 */

    if (!buffer || !len || len > 14)
        return -EINVAL;
    return spi_write_then_read(spi, &command, 1, buffer, len);
}

static int icm20608_write_reg(struct spi_device *spi, u8 reg, u8 value)
{
    u8 *tx;
    int ret;

    /* spi_write 的实际传输缓冲区不能直接用栈数组。 */
    tx = kmalloc(2, GFP_KERNEL);
    if (!tx)
        return -ENOMEM;
    tx[0] = reg & 0x7f; /* bit7=0：写寄存器 */
    tx[1] = value;
    ret = spi_write(spi, tx, 2);
    kfree(tx);
    return ret;
}

static int icm20608_hw_init(struct spi_device *spi)
{
    static const struct {
        u8 reg;
        u8 value;
    } settings[] = {
        { ICM20_USER_CTRL,     0x10 }, /* SPI 模式禁用 I2C 接口 */
        { ICM20_SMPLRT_DIV,    0x00 },
        { ICM20_GYRO_CONFIG,   0x18 }, /* ±2000 deg/s */
        { ICM20_ACCEL_CONFIG,  0x18 }, /* ±16 g */
        { ICM20_CONFIG,        0x04 },
        { ICM20_ACCEL_CONFIG2, 0x04 },
        { ICM20_PWR_MGMT_2,    0x00 }, /* 开启全部六轴 */
        { ICM20_LP_MODE_CFG,   0x00 },
        { ICM20_FIFO_EN,       0x00 },
        { ICM20_INT_ENABLE,    0x00 }, /* 本版使用轮询 */
    };
    u8 value;
    int ret, i;

    ret = icm20608_write_reg(spi, ICM20_PWR_MGMT_1, 0x80);
    if (ret)
        return ret;
    msleep(100);
    ret = icm20608_write_reg(spi, ICM20_PWR_MGMT_1, 0x01);
    if (ret)
        return ret;
    msleep(100);
    ret = icm20608_read_regs(spi, ICM20_WHO_AM_I, &value, 1);
    if (ret)
        return ret;
    dev_info(&spi->dev, "WHO_AM_I=0x%02x\n", value);
    if (value != ICM20608G_ID && value != ICM20608D_ID)
        return -ENODEV;

    for (i = 0; i < ARRAY_SIZE(settings); i++) {
        ret = icm20608_write_reg(spi, settings[i].reg, settings[i].value);
        if (ret)
            return ret;
    }
    /* 回读量程，防止写入失败却仍按既定量程解释数据。 */
    ret = icm20608_read_regs(spi, ICM20_GYRO_CONFIG, &value, 1);
    if (ret)
        return ret;
    if ((value & 0x18) != 0x18)
        return -EIO;
    ret = icm20608_read_regs(spi, ICM20_ACCEL_CONFIG, &value, 1);
    if (ret)
        return ret;
    if ((value & 0x18) != 0x18)
        return -EIO;
    msleep(100);
    return 0;
}

static s32 icm20608_decode(const u8 *bytes)
{
    /* 传感器寄存器是高字节在前；符号扩展到用户接口的 32 位。 */
    return (s16)(((u16)bytes[0] << 8) | bytes[1]);
}

static int icm20608_sample(struct spi_device *spi, s32 data[7])
{
    u8 bytes[14];
    int ret;

    ret = icm20608_read_regs(spi, ICM20_ACCEL_XOUT_H,
                             bytes, sizeof(bytes));
    if (ret)
        return ret;
    /* 保留旧应用接口顺序，和寄存器中的顺序不同。 */
    data[0] = icm20608_decode(bytes + 8);  /* GX */
    data[1] = icm20608_decode(bytes + 10); /* GY */
    data[2] = icm20608_decode(bytes + 12); /* GZ */
    data[3] = icm20608_decode(bytes);      /* AX */
    data[4] = icm20608_decode(bytes + 2);  /* AY */
    data[5] = icm20608_decode(bytes + 4);  /* AZ */
    data[6] = icm20608_decode(bytes + 6);  /* 芯片内部温度原始值 */
    return 0;
}

static int icm20608_open(struct inode *inode, struct file *file)
{
    int ret;

    ret = mutex_lock_interruptible(&icm_lock);
    if (ret)
        return ret;
    /* cdev 比较拒绝移除/重新绑定期间对旧 cdev 的迟到 open。 */
    if (!icmdev.present || inode->i_cdev != icmdev.cdev) {
        ret = -ENODEV;
        goto out;
    }
    if (icmdev.opened) {
        ret = -EBUSY;
        goto out;
    }
    if (file->f_mode & FMODE_WRITE) {
        ret = -EACCES;
        goto out;
    }
    ret = nonseekable_open(inode, file);
    if (!ret) {
        icmdev.opened = true;
        file->private_data = &icmdev;
    }
out:
    mutex_unlock(&icm_lock);
    return ret;
}

static ssize_t icm20608_read(struct file *file, char __user *buf,
                            size_t count, loff_t *pos)
{
    struct icm20608_dev *dev = file->private_data;
    s32 data[7];
    int ret;

    if (!count)
        return 0;
    if (count < sizeof(data))
        return -EINVAL;
    ret = mutex_lock_interruptible(&icm_lock);
    if (ret)
        return ret;
    if (!dev->present || !dev->spi)
        ret = -ENODEV;
    else
        ret = icm20608_sample(dev->spi, data);
    mutex_unlock(&icm_lock);
    if (ret)
        return ret;
    if (copy_to_user(buf, data, sizeof(data)))
        return -EFAULT;
    return sizeof(data);
}

static int icm20608_release(struct inode *inode, struct file *file)
{
    mutex_lock(&icm_lock);
    icmdev.opened = false;
    file->private_data = NULL;
    mutex_unlock(&icm_lock);
    return 0;
}

static const struct file_operations icm20608_fops = {
    .owner = THIS_MODULE,
    .open = icm20608_open,
    .read = icm20608_read,
    .release = icm20608_release,
    .llseek = no_llseek,
};

static int icm20608_probe(struct spi_device *spi)
{
    int ret;

    /* 默认设备树 MODE0；也接受正确声明的 MODE3，不支持其他模式。 */
    if (spi->mode != SPI_MODE_0 && spi->mode != SPI_MODE_3)
        return -EINVAL;
    spi->bits_per_word = 8;
    /* 首次验证保守使用不超过 1 MHz，并保留 DT 中更低的频率。 */
    if (!spi->max_speed_hz || spi->max_speed_hz > 1000000)
        spi->max_speed_hz = 1000000;
    ret = spi_setup(spi);
    if (ret)
        return ret;
    mutex_lock(&icm_lock);
    /* 只支持一颗芯片；旧 fd 未关闭时不允许新绑定复用其状态。 */
    if (icmdev.present || icmdev.opened) {
        ret = -EBUSY;
        goto out;
    }
    ret = icm20608_hw_init(spi);
    if (ret) {
        dev_err(&spi->dev, "initialization failed: %d\n", ret);
        goto power_down;
    }
    ret = alloc_chrdev_region(&icmdev.devid, 0, 1, ICM20608_NAME);
    if (ret)
        goto power_down;
    icmdev.class = class_create(THIS_MODULE, ICM20608_NAME);
    if (IS_ERR(icmdev.class)) {
        ret = PTR_ERR(icmdev.class);
        icmdev.class = NULL;
        goto unregister_region;
    }
    /* 独立分配 cdev，让其引用计数管理迟到 open/旧 inode 的生命周期。 */
    icmdev.cdev = cdev_alloc();
    if (!icmdev.cdev) {
        ret = -ENOMEM;
        goto destroy_class;
    }
    icmdev.cdev->owner = THIS_MODULE;
    icmdev.cdev->ops = &icm20608_fops;
    ret = cdev_add(icmdev.cdev, icmdev.devid, 1);
    if (ret) {
        kobject_put(&icmdev.cdev->kobj);
        icmdev.cdev = NULL;
        goto destroy_class;
    }
    icmdev.device = device_create(icmdev.class, &spi->dev, icmdev.devid,
                                 NULL, ICM20608_NAME);
    if (IS_ERR(icmdev.device)) {
        ret = PTR_ERR(icmdev.device);
        icmdev.device = NULL;
        goto delete_cdev;
    }
    icmdev.spi = spi;
    icmdev.present = true;
    spi_set_drvdata(spi, &icmdev);
    dev_info(&spi->dev,
             "ready: /dev/icm20608, ABI=s32[GX,GY,GZ,AX,AY,AZ,T], 28 bytes\n");
    mutex_unlock(&icm_lock);
    return 0;

delete_cdev:
    cdev_del(icmdev.cdev);
    icmdev.cdev = NULL;
destroy_class:
    class_destroy(icmdev.class);
    icmdev.class = NULL;
unregister_region:
    unregister_chrdev_region(icmdev.devid, 1);
power_down:
    /* 保留原始错误码。尽力关机，失败不掩盖首个错误。 */
    icm20608_write_reg(spi, ICM20_PWR_MGMT_1, 0x40);
out:
    mutex_unlock(&icm_lock);
    return ret;
}

static int icm20608_remove(struct spi_device *spi)
{
    int ret;

    mutex_lock(&icm_lock);
    icmdev.present = false;
    device_destroy(icmdev.class, icmdev.devid);
    cdev_del(icmdev.cdev);
    icmdev.cdev = NULL;
    class_destroy(icmdev.class);
    icmdev.class = NULL;
    icmdev.device = NULL;
    unregister_chrdev_region(icmdev.devid, 1);
    ret = icm20608_write_reg(spi, ICM20_PWR_MGMT_1, 0x40);
    if (ret)
        dev_warn(&spi->dev, "power down failed: %d\n", ret);
    icmdev.spi = NULL;
    spi_set_drvdata(spi, NULL);
    mutex_unlock(&icm_lock);
    return 0;
}

static const struct spi_device_id icm20608_ids[] = {
    { "icm20608", 0 },
    { "alientek,icm20608", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, icm20608_ids);

static const struct of_device_id icm20608_of_match[] = {
    { .compatible = "alientek,icm20608" },
    { }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
    .driver = {
        .name = ICM20608_NAME,
        .owner = THIS_MODULE,
        .of_match_table = icm20608_of_match,
    },
    .probe = icm20608_probe,
    .remove = icm20608_remove,
    .id_table = icm20608_ids,
};
module_spi_driver(icm20608_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("gongting");
MODULE_DESCRIPTION("ICM20608 SPI raw character interface for Linux 4.1.15");
