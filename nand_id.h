#ifndef _SUNXI_NAND_ID_H
#define _SUNXI_NAND_ID_H

struct nand_chip_param {
	unsigned char id[8];
	int id_len;
	int ecc_mode;   //the Ecc Mode for the nand flash chip, 0: bch-16, 1:bch-28, 2:bch_32
	int clock_freq; //the highest access frequence of the nand flash chip, based on MHz
};

extern struct nand_chip_param nand_chip_param[];

#endif
