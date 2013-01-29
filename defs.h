#ifndef _SUNXI_DEFS_H
#define _SUNXI_DEFS_H

#define PREFIX "[MTD][NAND][SUNXI]: "

#ifdef __LINUX__

#include <linux/kernel.h>

#define DBG_INFO(fmt, ...) printk(KERN_INFO PREFIX fmt, ##__VA_ARGS__)
#define ERR_INFO(fmt, ...) printk(KERN_ERR PREFIX fmt, ##__VA_ARGS__)

#else /* !__LINUX__ */

#define DBG_INFO(fmt, ...)
#define ERR_INFO(fmt, ...)

#endif

#endif
