#ifndef _SUNXI_NFC_H
#define _SUNXI_NFC_H

int nfc_first_init(struct mtd_info *info);
int nfc_second_init(struct mtd_info *info);
void nfc_exit(struct mtd_info *info);

#endif
