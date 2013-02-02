#ifndef _SUNXI_NAND_NFC_H
#define _SUNXI_NAND_NFC_H

int nfc_first_init(struct mtd_info *mtd);
int nfc_second_init(struct mtd_info *mtd);
void nfc_exit(struct mtd_info *mtd);

#endif
