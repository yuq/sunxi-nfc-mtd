#include <linux/io.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <plat/sys_config.h>

#include "defs.h"
#include "regs.h"
#include "dma.h"

// do we need to consider exclusion of offset?
// it should be in high level that the nand_chip ops have been
// performed with exclusion already
// find it is:
//   nand_get_device() & nand_release_device()
// 
static int read_offset = 0;
static int write_offset = 0;
static char *read_buffer = NULL;
static char *write_buffer = NULL;
static int buffer_size = 4096;
static dma_addr_t read_buffer_dma;
static dma_addr_t write_buffer_dma;
static int dma_hdle;
static struct nand_ecclayout sunxi_ecclayout;
static DECLARE_WAIT_QUEUE_HEAD(nand_rb_wait);

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
	static int program_column = -1, program_page = -1;
	uint32_t cfg = command;
	int addr_cycle, wait_rb_flag, byte_count, sector_count;
	addr_cycle = wait_rb_flag = byte_count = sector_count = 0;

	//DBG_INFO("command %x ...\n", command);
	wait_cmdfifo_free();

	// switch to AHB
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);

	switch (command) {
	case NAND_CMD_RESET:
	case NAND_CMD_ERASE2:
		break;
	case NAND_CMD_READID:
		addr_cycle = 1;
		// read 8 byte ID
		byte_count = 8;
		break;
	case NAND_CMD_PARAM:
		addr_cycle = 1;
		byte_count = 1024;
		wait_rb_flag = 1;
		break;
	case NAND_CMD_RNDOUT:
		addr_cycle = 2;
		writel(0xE0, NFC_REG_RCMD_SET);
		byte_count = 0x400;
		cfg |= NFC_SEND_CMD2;
		break;
	case NAND_CMD_READOOB:
	case NAND_CMD_READ0:
		if (command == NAND_CMD_READOOB) {
			cfg = NAND_CMD_READ0;
			// sector num to read
			sector_count = 1024 / 1024;
			// OOB offset
			column += mtd->writesize;
		}
		else
			sector_count = mtd->writesize / 1024;
			
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		// if the size is smaller than NFC_REG_SECTOR_NUM, read command won't finish
		// does that means the data read out (by DMA through random data output) hasn't finish?
		dma_nand_config_start(dma_hdle, 0, (uint32_t)read_buffer, sector_count * 1024);
		addr_cycle = 5;
		// RAM0 is 1K size
		byte_count =1024;
		wait_rb_flag = 1;
		// 0x30 for 2nd cycle of read page
		// 0x05+0xe0 is the random data output command
		writel(0x00e00530, NFC_REG_RCMD_SET);
		// NFC_SEND_CMD1 for the command 1nd cycle enable
		// NFC_SEND_CMD2 for the command 2nd cycle enable
		// NFC_SEND_CMD3 & NFC_SEND_CMD4 for NFC_READ_CMD0 & NFC_READ_CMD1
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD;
		// 3 - ?
		// 2 - page command
		// 1 - spare command?
		// 0 - normal command
		cfg |= 2 << 30;
		break;
	case NAND_CMD_ERASE1:
		addr_cycle = 3;
		break;
	case NAND_CMD_SEQIN:	
		program_column = column;
		program_page = page_addr;
		return;
	case NAND_CMD_PAGEPROG:
		cfg = NAND_CMD_SEQIN;
		addr_cycle = 5;
		column = program_column;
		page_addr = program_page;
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		dma_nand_config_start(dma_hdle, 1, (uint32_t)write_buffer, mtd->writesize);
		// RAM0 is 1K size
		byte_count =1024;
		writel(0x00008510, NFC_REG_WCMD_SET);
		cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_ACCESS_DIR;
		cfg |= 2 << 30;
		sector_count = mtd->writesize / 1024;
		break;
	case NAND_CMD_STATUS:
		byte_count = 1;
		break;
	default:
		ERR_INFO("unknown command\n");
		return;
	}

	// address cycle
	if (addr_cycle) {
		uint32_t low = 0;
		uint32_t high = 0;
		switch (addr_cycle) {
		case 2:
			low = column & 0xffff;
			break;
		case 3:
			low = page_addr & 0xffffff;
			break;
		case 5:
			high = (page_addr >> 16) & 0xff;
		case 4:
			low = (column & 0xffff) | (page_addr << 16);
			break;
		}
		writel(low, NFC_REG_ADDR_LOW);
		writel(high, NFC_REG_ADDR_HIGH);
		cfg |= NFC_SEND_ADR;
		cfg |= ((addr_cycle - 1) << 16);
	}

	// command will wait until the RB ready to mark finish?
	if (wait_rb_flag)
		cfg |= NFC_WAIT_FLAG;

	// will fetch data
	if (byte_count) {
		cfg |= NFC_DATA_TRANS;
		writel(byte_count, NFC_REG_CNT);
	}

	// set sectors
	if (sector_count)
		writel(sector_count, NFC_REG_SECTOR_NUM);

	// send command
	cfg |= NFC_SEND_CMD1;
	writel(cfg, NFC_REG_CMD);

	switch (command) {
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
	case NAND_CMD_PAGEPROG:
		dma_nand_wait_finish();
		break;
	}

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
	write_offset = 0;
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
	if (write_offset + len > buffer_size) {
		ERR_INFO("write too much\n");
		return;
	}
	memcpy(write_buffer + write_offset, buf, len);
	write_offset += len;
}

static void nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	if (read_offset + len > buffer_size) {
		ERR_INFO("read too much\n");
		return;
	}
	memcpy(buf, read_buffer + read_offset, len);
	read_offset += len;
}

static irqreturn_t nfc_interrupt_handler(int irq, void *dev_id)
{
	unsigned int st = readl(NFC_REG_ST);
	if (st & NFC_RB_B2R) {
		wake_up(&nand_rb_wait);
	}
	if (st & NFC_CMD_INT_FLAG) {
		DBG_INFO("CMD INT\n");
	}
	if (st & NFC_DMA_INT_FLAG) {
		DBG_INFO("DMA INT\n");
	}
	if (st & NFC_NATCH_INT_FLAG) {
		DBG_INFO("NATCH INT\n");
	}
	// clear interrupt
	writel(st, NFC_REG_ST);
	return IRQ_HANDLED;
}

static int get_chip_status(struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd->priv;
	nand->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);
	return nand->read_byte(mtd);
}

// For erase and program command to wait for chip ready
static int nfc_wait(struct mtd_info *mtd, struct nand_chip *chip)
{
	int err;

	// clear B2R interrupt state
	writel(NFC_RB_B2R, NFC_REG_ST);

	if (check_rb_ready(0))
		goto out;

	// enable B2R interrupt
	writel(NFC_B2R_INT_ENABLE, NFC_REG_INT);
	if ((err = wait_event_timeout(nand_rb_wait, check_rb_ready(0), 1*HZ)) < 0) {
		DBG_INFO("nfc wait got exception %d\n", err);
	}
	// disable interrupt
	writel(0, NFC_REG_INT);

out:
	return get_chip_status(mtd);
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
	nand->waitfunc = nfc_wait;
	return 0;
}

static void print_page(struct mtd_info *mtd, int page)
{
	char buff[6];
	nfc_cmdfunc(mtd, NAND_CMD_READ0, 0, page);
	nfc_read_buf(mtd, buff, 6);
	DBG_INFO("READ: %x %x %x %x %x %x\n",
			 buff[0], buff[1], buff[2], buff[3], buff[4], buff[5]);
}

static void test_nfc(struct mtd_info *mtd)
{
	struct nand_chip *nand = mtd->priv;
	int page = 1280;

	DBG_INFO("============== TEST NFC ================\n");

	// read page
	print_page(mtd, page);

	// erase block
	nfc_cmdfunc(mtd, NAND_CMD_ERASE1, 0, page);
	nfc_cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);
	nfc_wait(mtd, nand);
	print_page(mtd, page);

	// write block
	char buff[3] = {0x13, 0x5a, 0xc4};
	nfc_cmdfunc(mtd, NAND_CMD_SEQIN, 0, page);
	nfc_write_buf(mtd, buff, 3);
	nfc_cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
	nfc_wait(mtd, nand);
	print_page(mtd, page);
}

int nfc_second_init(struct mtd_info *mtd)
{
	int n, i, err;
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
		err = -EINVAL;
		goto out;
	}
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
		err = -ENODEV;
		goto out;
	}

	// alloc buffer
	buffer_size = mtd->writesize;
	read_buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (read_buffer == NULL) {
		ERR_INFO("alloc read buffer fail\n");
		err = -ENOMEM;
		goto release_dma_out;
	}
	write_buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (write_buffer == NULL) {
		ERR_INFO("alloc write buffer fail\n");
		err = -ENOMEM;
		goto free_read_out;
	}

	// map 
	read_buffer_dma = dma_map_single(NULL, read_buffer, buffer_size, DMA_FROM_DEVICE);
	write_buffer_dma = dma_map_single(NULL, write_buffer, buffer_size, DMA_TO_DEVICE);

	DBG_INFO("OOB size = %d  page size = %d  block size = %d  total size = %lld\n",
			 mtd->oobsize, mtd->writesize, mtd->erasesize, mtd->size);

	// register IRQ
	if ((err = request_irq(SW_INT_IRQNO_NAND, nfc_interrupt_handler, IRQF_DISABLED, "NFC", mtd)) < 0) {
		ERR_INFO("request IRQ fail\n");
		goto free_write_out;
	}

	// test command
	//test_nfc(mtd);

	return 0;

free_write_out:
	kfree(write_buffer);
free_read_out:
	kfree(read_buffer);
release_dma_out:
	dma_nand_release(dma_hdle);
out:
	return err;
}

void nfc_exit(struct mtd_info *mtd)
{
	free_irq(SW_INT_IRQNO_NAND, mtd);
	dma_unmap_single(NULL, read_buffer_dma, buffer_size, DMA_FROM_DEVICE);
	dma_unmap_single(NULL, write_buffer_dma, buffer_size, DMA_TO_DEVICE);
	dma_nand_release(dma_hdle);
	kfree(write_buffer);
	kfree(read_buffer);
	sunxi_release_nand_pio();
	release_nand_clock();
}

