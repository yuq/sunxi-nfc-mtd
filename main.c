#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <plat/sys_config.h>

#include "defs.h"
#include "nfc.h"

MODULE_LICENSE("GPL");

#define	DRIVER_NAME	"mtd-nand-sunxi"

struct sunxi_nand_info {
	struct mtd_info mtd;
	struct nand_chip nand;
};

static int __devinit nand_probe(struct platform_device *pdev)
{
	int err;
	struct sunxi_nand_info *info;

	if ((info = kzalloc(sizeof(*info), GFP_KERNEL)) == NULL) {
		ERR_INFO("alloc nand info fail\n");
		err = -ENOMEM;
		goto out;
	}

	info->mtd.priv = &info->nand;
	info->mtd.name = dev_name(&pdev->dev);
	info->mtd.owner = THIS_MODULE;

	if ((err = nfc_init(&info->mtd)) < 0) {
		ERR_INFO("nfc inti fail\n");
		goto out_free_info;
	}

	// first scan to find the device and get the page size
	if ((err = nand_scan_ident(&info->mtd, 1, NULL)) < 0) {
		ERR_INFO("nand scan ident fail\n");
		goto out_nfc_exit;
	}

	// init NFC with flash chip info got from first scan
	nfc_chip_init(&info->mtd);

	// second phase scan
	if ((err = nand_scan_tail(&info->mtd)) < 0) {
		ERR_INFO("nand scan tail fail\n");
		goto out_nfc_exit;
	}

	platform_set_drvdata(pdev, info);
	return 0;

 out_nfc_exit:
	nfc_exit(&info->mtd);
 out_free_info:
	kfree(info);
 out:
	return err;
}

static int __devexit nand_remove(struct platform_device *pdev)
{
	struct sunxi_nand_info *info = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	nand_release(&info->mtd);
	nfc_exit(&info->mtd);
	kfree(info);
	return 0;
}

static void nand_shutdown(struct platform_device *pdev)
{

}

static int nand_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int nand_resume(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver plat_driver = {
	.probe		= nand_probe,
	.remove		= nand_remove,
	.shutdown   = nand_shutdown,
	.suspend    = nand_suspend,
	.resume     = nand_resume,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
};

static struct platform_device plat_device = {
	.name = DRIVER_NAME,
	.id = 0,
};

static int __init nand_init(void)
{
	int err;
	int nand_used = 0;

    if (script_parser_fetch("nand_para", "nand_used", &nand_used, sizeof(int)))
    	ERR_INFO("nand init fetch emac using configuration failed\n");

    if(nand_used == 0) {
        DBG_INFO("nand driver is disabled \n");
        return 0;
    }

	DBG_INFO("nand driver, init.\n");

	if ((err = platform_driver_register(&plat_driver)) != 0) {
		ERR_INFO("platform_driver_register fail \n");
		return err;
	}
	DBG_INFO("nand driver, ok.\n");

	// add an NFC, may be should be done by platform driver
	if ((err = platform_device_register(&plat_device)) < 0) {
		ERR_INFO("platform_device_register fail\n");
		return err;
	}
	DBG_INFO("nand device, ok.\n");

	return 0;
}

static void __exit nand_exit(void)
{
	int nand_used = 0;

    if (script_parser_fetch("nand_para", "nand_used", &nand_used, sizeof(int)))
    	ERR_INFO("nand init fetch emac using configuration failed\n");

    if(nand_used == 0) {
        DBG_INFO("nand driver is disabled \n");
        return;
    }

	DBG_INFO("nand device : bye bye\n");
	platform_device_unregister(&plat_device);

	DBG_INFO("nand driver : bye bye\n");
	platform_driver_unregister(&plat_driver);
}

module_init(nand_init);
module_exit(nand_exit);




