#ifndef _SUNXI_NAND_DMA_H
#define _SUNXI_NAND_DMA_H

int dma_nand_request(unsigned int dmatype);
int dma_nand_release(int hDma);
void dma_nand_config_start(int dma, int rw, unsigned int buff_addr, size_t len);
int dma_nand_wait_finish(void);

#endif

