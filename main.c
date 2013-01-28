#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <plat/sys_config.h>

#include "defs.h"
#include "nfc.h"

#define	DRIVER_NAME	"mtd-nand-sunxi"

MODULE_LICENSE("GPL");

static int __devinit nand_probe(struct platform_device *pdev)
{
	//nfc_init();
	return 0;
}

static int __devexit nand_remove(struct platform_device *pdev)
{
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

static struct platform_driver nand_driver = {
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

	if ((err = platform_driver_register(&nand_driver)) != 0) {
		ERR_INFO("platform_driver_register fail \n");
		return err;
	}
	DBG_INFO("nand driver, ok.\n");

	nfc_init();
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

	DBG_INFO("nand driver : bye bye\n");
	platform_driver_unregister(&nand_driver);
}

module_init(nand_init);
module_exit(nand_exit);




