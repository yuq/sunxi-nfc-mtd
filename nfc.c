#ifdef __LINUX__
#include <linux/io.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <plat/sys_config.h>
#else

#endif

#include "defs.h"
#include "regs.h"
#include "dma.h"

// do we need to consider exclusion of offset?
// it should be in high level that the nand_chip ops have been
// performed with exclusion already
static int read_offset = 0;
static char *main_buffer;
static int main_buffer_size = 1024 * 8;
static dma_addr_t main_buffer_dma;
static int dma_hdle;
static struct nand_ecclayout sunxi_ecclayout;

//////////////////////////////////////////////////////////////////
// SUNXI platform
//

// Get clock rate from PLL5
static uint32_t sunxi_get_pll5_clk(void)
{
	uint32_t reg_val;
	uint32_t div_p, factor_n;
	uint32_t factor_k, factor_m;
	uint32_t clock;

	reg_val  = readl(PLL5_CFG_REG);
	div_p    = (reg_val & PLL5_OUT_EXT_DIV_P_MASK) >> PLL5_OUT_EXT_DIV_P_SHIFT;
	factor_n = (reg_val & PLL5_FACTOR_N_MASK) >> PLL5_FACTOR_N_SHIFT;
	factor_k = ((reg_val & PLL5_FACTOR_K_MASK) >> PLL5_FACTOR_K_SHIFT) + 1;
	factor_m = ((reg_val & PLL5_FACTOR_M_MASK) >> PLL5_FACTOR_M_SHIFT) + 1;

	clock = 24 * factor_n * factor_k / div_p / factor_m;
	DBG_INFO("cmu_clk is %d \n", clock);

	return clock;
}

static void sunxi_set_nand_clock(uint32_t nand_max_clock)
{
    uint32_t edo_clk, cmu_clk;
	uint32_t cfg;
	uint32_t nand_clk_divid_ratio;

	// open ahb nand clk (bus clock for CPU access)
	cfg = readl(AHB_GATING_REG0);
	cfg |= 1 << AHB_GATING_NAND_CLK_SHIFT;
	writel(cfg, AHB_GATING_REG0);

	// set nand clock (device clock for NFC running)
	edo_clk = nand_max_clock * 2;

    cmu_clk = sunxi_get_pll5_clk();
	nand_clk_divid_ratio = cmu_clk / edo_clk;
	if (cmu_clk % edo_clk)
		nand_clk_divid_ratio++;
	if (nand_clk_divid_ratio) {
		if (nand_clk_divid_ratio > 16)
			nand_clk_divid_ratio = 15;
		else
			nand_clk_divid_ratio--;
	}

	// set nand clock gate on
	cfg = readl(NAND_SCLK_CFG_REG);
	// gate on nand clock
	cfg |= 1 << SCLK_GATING_SHIFT;
	// take cmu pll as nand src block
	cfg &= ~CLK_SRC_SEL_MASK;
	cfg |= 0x2 << CLK_SRC_SEL_SHIFT;
	// set divn = 0
	cfg &= ~CLK_DIV_RATIO_N_MASK;
	// set divm
	cfg &= ~CLK_DIV_RATIO_M_MASK;
	cfg |= (nand_clk_divid_ratio << CLK_DIV_RATIO_M_SHIFT) & CLK_DIV_RATIO_M_MASK;
	writel(cfg, NAND_SCLK_CFG_REG);

	DBG_INFO("nand clk init end \n");
	DBG_INFO("offset 0xc:  0x%x \n", readl(AHB_GATING_REG0));
	DBG_INFO("offset 0x14:  0x%x \n", readl(NAND_SCLK_CFG_REG));
}

static void release_nand_clock(void)
{
	uint32_t cfg;

	// disable bus clock
	cfg = readl(AHB_GATING_REG0);
	cfg &= ~(1 << AHB_GATING_NAND_CLK_SHIFT);
	writel(cfg, AHB_GATING_REG0);

	// disable device clock
	cfg = readl(NAND_SCLK_CFG_REG);
	cfg &= ~(1 << SCLK_GATING_SHIFT);
	writel(cfg, NAND_SCLK_CFG_REG);
}

static void active_nand_clock(void)
{
	uint32_t cfg;

	// disable bus clock
	cfg = readl(AHB_GATING_REG0);
	cfg |= 1 << AHB_GATING_NAND_CLK_SHIFT;
	writel(cfg, AHB_GATING_REG0);

	// disable device clock
	cfg = readl(NAND_SCLK_CFG_REG);
	cfg |= 1 << SCLK_GATING_SHIFT;
	writel(cfg, NAND_SCLK_CFG_REG);
}

#ifdef __LINUX__
uint32_t pioc_handle;
#endif

// Set PIOC pin for NAND Flash use
static void sunxi_set_nand_pio(void)
{
#ifdef __LINUX__
	pioc_handle = gpio_request_ex("nand_para", NULL);
	if (pioc_handle) {
		DBG_INFO("get nand pio ok\n");
	}
	else {
		ERR_INFO("get nand pio fail\n");
	}
#else
	writel(0x22222222, PC_CFG0_REG);
	writel(0x22222222, PC_CFG1_REG);
	writel(0x22222222, PC_CFG2_REG);
#endif
}

static void sunxi_release_nand_pio(void)
{
#ifdef __LINUX__
	DBG_INFO("nand gpio_release\n");
	gpio_release(pioc_handle, 1);
#else
	writel(0, PC_CFG0_REG);
	writel(0, PC_CFG1_REG);
	writel(0, PC_CFG2_REG);
#endif
}

/////////////////////////////////////////////////////////////////
// Utils
//

static inline void wait_cmdfifo_free(void)
{
	while (readl(NFC_REG_ST) & NFC_CMD_FIFO_STATUS);
}

static inline void wait_cmd_finish(void)
{
	while(!(readl(NFC_REG_ST) & NFC_CMD_INT_FLAG));
	writel(NFC_CMD_INT_FLAG, NFC_REG_ST);
}

static void select_rb(int rb)
{
	uint32_t ctl;
	// A10 has 2 RB pin
	ctl = readl(NFC_REG_CTL);
	ctl &= ~NFC_RB_SEL;
	ctl |= ((rb & 0x1) << 3);
	writel(ctl, NFC_REG_CTL);
}

// 1 for ready, 0 for not ready
static inline int check_rb_ready(int rb)
{
	return (readl(NFC_REG_ST) & (NFC_RB_STATE0 << (rb & 0x3))) ? 1 : 0;
}

static void enable_random(void)
{
	uint32_t ctl;
	ctl = readl(NFC_REG_ECC_CTL);
	ctl |= NFC_RANDOM_EN;
	writel(ctl, NFC_REG_ECC_CTL);
}

static void disable_random(void)
{
	uint32_t ctl;
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_RANDOM_EN;
	writel(ctl, NFC_REG_ECC_CTL);
}

static void enable_ecc(int pipline)
{
	uint32_t cfg = readl(NFC_REG_ECC_CTL);
	if (pipline)
		cfg |= NFC_ECC_PIPELINE;
	else
		cfg &= (~NFC_ECC_PIPELINE) & 0xffffffff;

	// if random open, disable exception
	if(cfg & (1 << 9))
	    cfg &= ~(0x1 << 4);
	else
	    cfg |= 1 << 4;

	//cfg |= (1 << 1); 16 bit ecc

	cfg |= NFC_ECC_EN;
	writel(cfg, NFC_REG_ECC_CTL);
}

static void disable_ecc(void)
{
	uint32_t cfg = readl(NFC_REG_ECC_CTL);
	cfg &= (~NFC_ECC_EN) & 0xffffffff;
	writel(cfg, NFC_REG_ECC_CTL);
}

/////////////////////////////////////////////////////////////////
// NFC
//

static void nfc_select_chip(struct mtd_info *mtd, int chip)
{
	uint32_t ctl;
	// A10 has 8 CE pin to support 8 flash chips
    ctl = readl(NFC_REG_CTL);
    ctl &= ~NFC_CE_SEL;
	ctl |= ((chip & 7) << 24);
    writel(ctl, NFC_REG_CTL);
}

static void nfc_cmdfunc(struct mtd_info *mtd, unsigned command, int column,
						int page_addr)
{
	uint32_t cfg = command;
	int addr_cycle, wait_rb_flag, data_fetch_flag, byte_count;
	addr_cycle = wait_rb_flag = data_fetch_flag = 0;

	//DBG_INFO("command %x ...\n", command);
	wait_cmdfifo_free();

	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);

	switch (command) {
	case NAND_CMD_RESET:
	case NAND_CMD_ERASE2:
		break;
	case NAND_CMD_READID:
		writel(column, NFC_REG_ADDR_LOW);
		writel(0, NFC_REG_ADDR_HIGH);
		addr_cycle = 1;
		data_fetch_flag = 1;
		// read 8 byte ID
		byte_count = 8;
		break;
	case NAND_CMD_PARAM:
		writel(column, NFC_REG_ADDR_LOW);
		writel(0, NFC_REG_ADDR_HIGH);
		addr_cycle = 1;
		data_fetch_flag = 1;
		byte_count = 1024;
		wait_rb_flag = 1;
		break;
	case NAND_CMD_RNDOUT:
		writel(column, NFC_REG_ADDR_LOW);
		writel(0, NFC_REG_ADDR_HIGH);
		addr_cycle = 2;
		writel(0xE0, NFC_REG_RCMD_SET);
		data_fetch_flag = 1;
		byte_count = 0x400;
		cfg |= NFC_SEND_CMD2;
		break;
	case NAND_CMD_READOOB:
		cfg = NAND_CMD_READ0;
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		dma_nand_config_start(dma_hdle, 0, (uint32_t)main_buffer, 1024);
		writel(((column + 4096) & 0xffff) | ((page_addr & 0xffff) << 16), NFC_REG_ADDR_LOW);
		writel((page_addr >> 16) & 0xff, NFC_REG_ADDR_HIGH);
		addr_cycle = 5;
		data_fetch_flag = 1;
		// RAM0 is 1K size
		byte_count =1024;
		wait_rb_flag = 1;
		writel(0x00e00530, NFC_REG_RCMD_SET);
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD;
		// page command
		cfg |= 2 << 30;
		// sector num to read
		writel(1024 / 1024, NFC_REG_SECTOR_NUM);
		break;
	case NAND_CMD_READ0:
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		dma_nand_config_start(dma_hdle, 0, (uint32_t)main_buffer, 4096);
		writel((column & 0xffff) | ((page_addr & 0xffff) << 16), NFC_REG_ADDR_LOW);
		writel((page_addr >> 16) & 0xff, NFC_REG_ADDR_HIGH);
		addr_cycle = 5;
		data_fetch_flag = 1;
		// RAM0 is 1K size
		byte_count =1024;
		wait_rb_flag = 1;
		writel(0x00e00530, NFC_REG_RCMD_SET);
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD;
		// 3 - ?
		// 2 - page command
		// 1 - spare command?
		// 0 - normal command
		cfg |= 2 << 30;
		writel(4096 / 1024, NFC_REG_SECTOR_NUM);
		break;
	case NAND_CMD_ERASE1:
		addr_cycle = 3;
		writel(page_addr, NFC_REG_ADDR_LOW);
		writel(0, NFC_REG_ADDR_HIGH);
		break;
	case NAND_CMD_PAGEPROG:
		cfg = 0x80;
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		dma_nand_config_start(dma_hdle, 1, (uint32_t)main_buffer, 4096);
		writel((column & 0xffff) | ((page_addr & 0xffff) << 16), NFC_REG_ADDR_LOW);
		writel((page_addr >> 16) & 0xff, NFC_REG_ADDR_HIGH);
		addr_cycle = 5;
		data_fetch_flag = 1;
		// RAM0 is 1K size
		byte_count =1024;
		writel(0x00008510, NFC_REG_WCMD_SET);
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_ACCESS_DIR;
		// 3 - ?
		// 2 - page command
		// 1 - spare command?
		// 0 - normal command
		cfg |= 2 << 30;
		writel(4096 / 1024, NFC_REG_SECTOR_NUM);
		break;
	case NAND_CMD_STATUS:
		data_fetch_flag = 1;
		byte_count = 1;
		break;
	default:
		ERR_INFO("unknown command\n");
		return;
	}

	// send command
	cfg |= NFC_SEND_CMD1;
	if (addr_cycle > 0) {
		cfg |= NFC_SEND_ADR;
		cfg |= ((addr_cycle - 1) << 16);
	}
	if (wait_rb_flag)
		cfg |= NFC_WAIT_FLAG;
	if (data_fetch_flag) {
		cfg |= NFC_DATA_TRANS;
		writel(byte_count, NFC_REG_CNT);
	}
	writel(cfg, NFC_REG_CMD);

	if (command == NAND_CMD_READ0 || command == NAND_CMD_PAGEPROG)
		dma_nand_wait_finish();

	// wait command send complete
	wait_cmdfifo_free();
	wait_cmd_finish();

	// reset will wait for RB ready
	if (command == NAND_CMD_RESET) {
		// wait rb0 ready
		select_rb(0);
		while (!check_rb_ready(0));
		// wait rb1 ready
		select_rb(1);
		while (!check_rb_ready(1));
		// select rb 0 back
		select_rb(0);
	}

	//DBG_INFO("done\n");

	// reset read write offset
	read_offset = 0;
}

static uint8_t nfc_read_byte(struct mtd_info *mtd)
{
	return readb(NFC_RAM0_BASE + read_offset++);
}

static int nfc_dev_ready(struct mtd_info *mtd)
{
	return check_rb_ready(0);
}

static void nfc_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{

}

static void nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	memcpy(buf, main_buffer + read_offset, len);
	read_offset += len;
}

int nfc_first_init(struct mtd_info *mtd)
{
	uint32_t ctl;
	struct nand_chip *nand = mtd->priv;

	// set NFC clock source
	sunxi_set_nand_clock(20);

	// set NFC pio
	sunxi_set_nand_pio();

	// reset NFC
	ctl = readl(NFC_REG_CTL);
	ctl |= NFC_RESET;
	writel(ctl, NFC_REG_CTL);
	while(readl(NFC_REG_CTL) & NFC_RESET);

	// enable NFC
	ctl = NFC_EN;
	writel(ctl, NFC_REG_CTL);

	nand->ecc.mode = NAND_ECC_SOFT;
	nand->select_chip = nfc_select_chip;
	nand->dev_ready = nfc_dev_ready;
	nand->cmdfunc = nfc_cmdfunc;
	nand->read_byte = nfc_read_byte;
	nand->read_buf = nfc_read_buf;
	nand->write_buf = nfc_write_buf;
	//nand->IO_ADDR_R = NFC_RAM0_BASE + 8;
	//nand->IO_ADDR_W = NFC_RAM0_BASE + 8;
	return 0;
}

int nfc_second_init(struct mtd_info *mtd)
{
	int n, i;
	uint32_t ctl;
	struct nand_chip *nand = mtd->priv;

	// disable interrupt
	writel(0, NFC_REG_INT);
	// clear interrupt
	writel(readl(NFC_REG_ST), NFC_REG_ST);

	// set ECC mode
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	writel(ctl, NFC_REG_ECC_CTL);

	// enable NFC
	ctl = NFC_EN;

	// Bus width
	if (nand->options & NAND_BUSWIDTH_16) {
		DBG_INFO("flash chip bus width 16\n");
		ctl |= (1 & 0x1) << 2;
	}
	else {
		DBG_INFO("flash chip bus width 8\n");
	}

	// Page size
	if (nand->page_shift > 14 || nand->page_shift < 10) {
		ERR_INFO("Flash chip page shift out of range %d\n", nand->page_shift);
		return -EINVAL;
	}
	DBG_INFO("flash chip page shift %d\n", nand->page_shift);
	// 0 for 1K
	ctl |= ((nand->page_shift - 10) & 0xf) << 8;
	writel(ctl, NFC_REG_CTL);

	ctl = (1 << 8); /* serial_access_mode = 1 */
	writel(ctl, NFC_REG_TIMING_CTL);
	writel(0xff, NFC_REG_TIMING_CFG);
	writel(1 << nand->page_shift, NFC_REG_SPARE_AREA);

	// disable random
	disable_random();

	// setup ECC layout for BCH8
	n = (nand->page_shift - 9) * 14;
	sunxi_ecclayout.eccbytes = n;
	for (i = 0; i < n; i++)
		sunxi_ecclayout.eccpos[i] = i + 2;
	sunxi_ecclayout.oobfree->offset = n + 2;
	sunxi_ecclayout.oobfree->length = mtd->oobsize - n - 2;
	nand->ecc.layout = &sunxi_ecclayout;

	// setup DMA
	dma_hdle = dma_nand_request(1);
	if (dma_hdle == 0) {
		ERR_INFO("request DMA fail\n");
		return -ENODEV;
	}

	// alloc buffer
	main_buffer = kmalloc(main_buffer_size, GFP_KERNEL);
	if (main_buffer == NULL) {
		ERR_INFO("alloc main buffer fail\n");
		return -ENOMEM;
	}
	main_buffer_dma = dma_map_single(NULL, main_buffer, main_buffer_size, DMA_FROM_DEVICE);

	int offset = 0;
	int page = 1281;

	memset(main_buffer, 0xff, 4096);
	nfc_cmdfunc(mtd, NAND_CMD_READ0, 0, page);
	DBG_INFO("Read %x %x %x %x %x %x\n",
			 main_buffer[offset], main_buffer[offset + 1], 
			 main_buffer[offset + 2], main_buffer[offset + 3], 
			 main_buffer[offset + 4], main_buffer[offset+ 5]);

	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));

	offset = 0;
	memset(main_buffer, 0xff, 4096);
	nfc_cmdfunc(mtd, NAND_CMD_READOOB, 0, page);
	DBG_INFO("Read %x %x %x %x %x %x\n",
			 main_buffer[offset], main_buffer[offset + 1], 
			 main_buffer[offset + 2], main_buffer[offset + 3], 
			 main_buffer[offset + 4], main_buffer[offset+ 5]);

	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));

	memset(main_buffer, 0xff, 4096);
	memset(main_buffer, 0xbf, 4);
	nfc_cmdfunc(mtd, NAND_CMD_ERASE1, 0, page);
	while (!check_rb_ready(0));
	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));
	nfc_cmdfunc(mtd, NAND_CMD_ERASE2, 0, page);
	while (!check_rb_ready(0));
	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));
	//nfc_cmdfunc(mtd, NAND_CMD_PAGEPROG, 0, page);
	//while (!check_rb_ready(0));
	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));

	offset = 0;
	memset(main_buffer, 0xff, 4096);
	nfc_cmdfunc(mtd, NAND_CMD_READ0, 0, page);
	DBG_INFO("Read %x %x %x %x %x %x\n",
			 main_buffer[offset], main_buffer[offset + 1], 
			 main_buffer[offset + 2], main_buffer[offset + 3], 
			 main_buffer[offset + 4], main_buffer[offset+ 5]);

	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));

	offset = 0;
	memset(main_buffer, 0xff, 4096);
	nfc_cmdfunc(mtd, NAND_CMD_READOOB, 0, page);
	DBG_INFO("Read %x %x %x %x %x %x\n",
			 main_buffer[offset], main_buffer[offset + 1], 
			 main_buffer[offset + 2], main_buffer[offset + 3], 
			 main_buffer[offset + 4], main_buffer[offset+ 5]);

	//nfc_cmdfunc(mtd, NAND_CMD_STATUS, 0, 0);
	//DBG_INFO("status: %x\n", nfc_read_byte(mtd));

	DBG_INFO("OOB size = %d  page size = %d  block size = %d  total size = %lld\n",
			 mtd->oobsize, mtd->writesize, mtd->erasesize, mtd->size);
	return 0;
}

void nfc_exit(struct mtd_info *mtd)
{
	dma_unmap_single(NULL, main_buffer_dma, 8192, DMA_FROM_DEVICE);
	dma_nand_release(dma_hdle);
	kfree(main_buffer);
	sunxi_release_nand_pio();
	release_nand_clock();
}

