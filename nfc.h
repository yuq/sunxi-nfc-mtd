#ifndef _SUNXI_NFC_H
#define _SUNXI_NFC_H

int nfc_init(struct mtd_info *info);
void nfc_chip_init(struct mtd_info *info);
void nfc_exit(struct mtd_info *info);

#endif
