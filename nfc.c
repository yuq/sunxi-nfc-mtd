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
#include "nand_id.h"

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
static int buffer_size = 8192 + 1024;
static dma_addr_t read_buffer_dma;
static dma_addr_t write_buffer_dma;
static int dma_hdle;
static struct nand_ecclayout sunxi_ecclayout;
static DECLARE_WAIT_QUEUE_HEAD(nand_rb_wait);
static int program_column = -1, program_page = -1;

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
	int timeout = 0xffff;
	while ((timeout--) && (readl(NFC_REG_ST) & NFC_CMD_FIFO_STATUS));
	if (timeout <= 0) {
		ERR_INFO("wait_cmdfifo_free timeout\n");
	}
}

static inline void wait_cmd_finish(void)
{
	int timeout = 0xffff;
	while((timeout--) && !(readl(NFC_REG_ST) & NFC_CMD_INT_FLAG));
	if (timeout <= 0) {
		ERR_INFO("wait_cmd_finish timeout\n");
		return;
	}
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

int check_ecc(int eblock_cnt)
{
	int i;
    int ecc_mode;
	int max_ecc_bit_cnt = 16;
	int cfg;
    uint8_t ecc_cnt[16];

	ecc_mode = (readl(NFC_REG_ECC_CTL) & NFC_ECC_MODE) >> NFC_ECC_MODE_SHIFT;
	if(ecc_mode == 0)
		max_ecc_bit_cnt = 16;
	if(ecc_mode == 1)
		max_ecc_bit_cnt = 24;
	if(ecc_mode == 2)
		max_ecc_bit_cnt = 28;
	if(ecc_mode == 3)
		max_ecc_bit_cnt = 32;
	if(ecc_mode == 4)
		max_ecc_bit_cnt = 40;
	if(ecc_mode == 5)
		max_ecc_bit_cnt = 48;
	if(ecc_mode == 6)
		max_ecc_bit_cnt = 56;
    if(ecc_mode == 7)
		max_ecc_bit_cnt = 60;
    if(ecc_mode == 8)
		max_ecc_bit_cnt = 64;

	//check ecc errro
	cfg = readl(NFC_REG_ECC_ST) & 0xffff;
	for (i = 0; i < eblock_cnt; i++) {
		if (cfg & (1<<i))
			return -1;
	}

	//check ecc limit
	cfg = readl(NFC_REG_ECC_CNT0);
	ecc_cnt[0] = (uint8_t)((cfg>>0)&0xff);
	ecc_cnt[1] = (uint8_t)((cfg>>8)&0xff);
	ecc_cnt[2] = (uint8_t)((cfg>>16)&0xff);
	ecc_cnt[3] = (uint8_t)((cfg>>24)&0xff);

	cfg = readl(NFC_REG_ECC_CNT1);
	ecc_cnt[4] = (uint8_t)((cfg>>0)&0xff);
	ecc_cnt[5] = (uint8_t)((cfg>>8)&0xff);
	ecc_cnt[6] = (uint8_t)((cfg>>16)&0xff);
	ecc_cnt[7] = (uint8_t)((cfg>>24)&0xff);

	cfg = readl(NFC_REG_ECC_CNT2);
	ecc_cnt[8] = (uint8_t)((cfg>>0)&0xff);
	ecc_cnt[9] = (uint8_t)((cfg>>8)&0xff);
	ecc_cnt[10] = (uint8_t)((cfg>>16)&0xff);
	ecc_cnt[11] = (uint8_t)((cfg>>24)&0xff);

	cfg = readl(NFC_REG_ECC_CNT3);
	ecc_cnt[12] = (uint8_t)((cfg>>0)&0xff);
	ecc_cnt[13] = (uint8_t)((cfg>>8)&0xff);
	ecc_cnt[14] = (uint8_t)((cfg>>16)&0xff);
	ecc_cnt[15] = (uint8_t)((cfg>>24)&0xff);

	for (i = 0; i < eblock_cnt; i++) {
		if((max_ecc_bit_cnt - 4) <= ecc_cnt[i])
			return 1;
	}

	return 0;
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
	int i;
	uint32_t cfg = command;
	int read_size, do_enable_ecc = 0;
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
			read_size = 1024;
			// OOB offset
			column += mtd->writesize;
		}
		else {
			sector_count = mtd->writesize / 1024;
			read_size = mtd->writesize;
			do_enable_ecc = 1;
		}
			
		//access NFC internal RAM by DMA bus
		writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);
		// if the size is smaller than NFC_REG_SECTOR_NUM, read command won't finish
		// does that means the data read out (by DMA through random data output) hasn't finish?
		dma_nand_config_start(dma_hdle, 0, (uint32_t)read_buffer, read_size);
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
		write_offset = 0;
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
		do_enable_ecc = 1;
		for (i = 0; i < 80/4; i++)
			writel(*((unsigned int *)(write_buffer + mtd->oobsize) + i), NFC_REG_USER_DATA(i));
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

	// enable ecc
	if (do_enable_ecc)
		enable_ecc(1);

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
	switch (command) {
	case NAND_CMD_RESET:
		// wait rb0 ready
		select_rb(0);
		while (!check_rb_ready(0));
		// wait rb1 ready
		select_rb(1);
		while (!check_rb_ready(1));
		// select rb 0 back
		select_rb(0);
		break;
	case NAND_CMD_READ0:
		printk(KERN_INFO "USRDATA: ");
		for (i = 0; i < 80/4; i++) {
			*((unsigned int *)(read_buffer + mtd->oobsize) + i) = readl(NFC_REG_USER_DATA(i));
			printk("%02x %02x %02x %02x ", 
				   read_buffer[mtd->oobsize + i*4], 
				   read_buffer[mtd->oobsize + i*4 + 1], 
				   read_buffer[mtd->oobsize + i*4 + 2], 
				   read_buffer[mtd->oobsize + i*4 + 3]);
		}
		printk("\n");
		if (check_ecc(sector_count))
			ERR_INFO("ECC check fail\n");
		break;
	}

	if (do_enable_ecc)
		disable_ecc();

	//DBG_INFO("done\n");

	// read write offset
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
	if (write_offset + len > buffer_size) {
		ERR_INFO("write too much offset=%d len=%d buffer size=%d\n",
				 write_offset, len, buffer_size);
		return;
	}
	memcpy(write_buffer + write_offset, buf, len);
	write_offset += len;
}

static void nfc_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	if (read_offset + len > buffer_size) {
		ERR_INFO("read too much offset=%d len=%d buffer size=%d\n", 
				 read_offset, len, buffer_size);
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

static int nfc_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip, uint8_t *buf, int page)
{
	int err = 0;
	uint32_t cfg = NAND_CMD_READ0;

	wait_cmdfifo_free();

	//access NFC internal RAM by DMA bus
	writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);

	dma_nand_config_start(dma_hdle, 0, (uint32_t)read_buffer, mtd->writesize);

	// read command
	writel(0x00e00530, NFC_REG_RCMD_SET);

	// address
	writel(page << 16, NFC_REG_ADDR_LOW);
	writel(page >> 16, NFC_REG_ADDR_HIGH);
	cfg |= 4 << 16;

	// byte count
	writel(1024, NFC_REG_CNT);
	writel(mtd->writesize / 1024, NFC_REG_SECTOR_NUM);

	// command attr
	cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_SEND_ADR | 
		NFC_WAIT_FLAG | NFC_DATA_TRANS | NFC_SEND_CMD1 |
		NFC_DATA_SWAP_METHOD | (2 << 30);

	// enable ecc
	enable_ecc(1);

	// send command
	writel(cfg, NFC_REG_CMD);

	// wait DMA
	dma_nand_wait_finish();

	// wait command send complete
	wait_cmdfifo_free();
	wait_cmd_finish();

	if (check_ecc(mtd->writesize / 1024)) {
		ERR_INFO("ECC check fail\n");
		err = -1;
	}

	// disable ecc
	disable_ecc();

	// switch to AHB
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);

	// Can we just DMA the data directly into buff?
	// The buff must be 4 bytes aligned (can we ensure this?)
	if (!err)
		memcpy(buf, read_buffer, mtd->writesize);

	return err;
}

static void nfc_write_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip, const uint8_t *buf)
{
	uint32_t cfg = NAND_CMD_SEQIN;

	// If buf is 4 bytes aligned, we can directly DMA it
	memcpy(write_buffer, buf, mtd->writesize);

	wait_cmdfifo_free();

	//access NFC internal RAM by DMA bus
	writel(readl(NFC_REG_CTL) | NFC_RAM_METHOD, NFC_REG_CTL);

	dma_nand_config_start(dma_hdle, 1, (uint32_t)write_buffer, mtd->writesize);

	// write command
	writel(0x00008510, NFC_REG_WCMD_SET);

	// address
	writel(program_page << 16, NFC_REG_ADDR_LOW);
	writel(program_page >> 16, NFC_REG_ADDR_HIGH);
	cfg |= 4 << 16;

	// byte count
	writel(1024, NFC_REG_CNT);
	writel(mtd->writesize / 1024, NFC_REG_SECTOR_NUM);

	// command attr
	cfg |= NFC_SEND_CMD2 | NFC_DATA_SWAP_METHOD | NFC_SEND_ADR | 
		NFC_WAIT_FLAG | NFC_DATA_TRANS | NFC_SEND_CMD1 |
		NFC_DATA_SWAP_METHOD | NFC_ACCESS_DIR | (2 << 30);

	// enable ecc
	enable_ecc(1);

	// send command
	writel(cfg, NFC_REG_CMD);

	// wait DMA
	dma_nand_wait_finish();

	// wait command send complete
	wait_cmdfifo_free();
	wait_cmd_finish();

	// disable ecc
	disable_ecc();

	// switch to AHB
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);
}

static void first_test_nfc(struct mtd_info *mtd)
{
	nfc_cmdfunc(mtd, NAND_CMD_RESET, -1, -1);
	DBG_INFO("reset\n");
	DBG_INFO("nand ctrl %x\n", readl(NFC_REG_CTL));
	DBG_INFO("nand ecc ctrl %x\n", readl(NFC_REG_ECC_CTL));
	DBG_INFO("nand timing %x\n", readl(NFC_REG_TIMING_CTL));
	nfc_cmdfunc(mtd, NAND_CMD_READID, 0, -1);
	DBG_INFO("readid first time: %x %x\n", 
			 nfc_read_byte(mtd),  nfc_read_byte(mtd));
	nfc_cmdfunc(mtd, NAND_CMD_READID, 0, -1);
	DBG_INFO("readid second time: %x %x\n", 
			 nfc_read_byte(mtd),  nfc_read_byte(mtd));
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

	// serial_access_mode = 1
	// this is needed by some nand chip to read ID
	ctl = (1 << 8);
	writel(ctl, NFC_REG_TIMING_CTL);

	//first_test_nfc(mtd);

	nand->ecc.mode = NAND_ECC_HW;
	nand->ecc.read_page = nfc_read_page_hwecc;
	nand->ecc.write_page = nfc_write_page_hwecc;
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
	int i;
	char buff[1024];
	nfc_cmdfunc(mtd, NAND_CMD_READ0, 0, page);
	nfc_read_buf(mtd, buff, 6);
	DBG_INFO("READ: %x %x %x %x %x %x\n",
			 buff[0], buff[1], buff[2], buff[3], buff[4], buff[5]);

	nfc_cmdfunc(mtd, NAND_CMD_READOOB, 0, page);
	nfc_read_buf(mtd, buff, 640);
	for (i = 0; i < 640; i++)
		printk("%02x ", buff[i]);
	printk("\n");
}

static void test_nfc(struct mtd_info *mtd)
{
	int i, j, n=0;
	struct nand_chip *nand = mtd->priv;
	int page = 1280;
	unsigned char buff[1024];
	int blocks = 2, num_blocks = mtd->writesize / 1024;
	static unsigned char buffer[8192];

	DBG_INFO("============== TEST NFC ================\n");

	// read page
	print_page(mtd, page);

	// erase block
	nfc_cmdfunc(mtd, NAND_CMD_ERASE1, 0, page);
	nfc_cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);
	nfc_wait(mtd, nand);
	print_page(mtd, page);

	// write block
	nfc_cmdfunc(mtd, NAND_CMD_SEQIN, 0, page);
	for (i = 0; i < blocks; i++) {
		for (j = 0; j < 1024; j++, n++)
			buff[j] = n % 256;
		nfc_write_buf(mtd, buff, 1024);
	}
	for ( ; i < num_blocks; i++) {
		memset(buff, 0xff, 1024);
		nfc_write_buf(mtd, buff, 1024);
	}
	// wrong mtd->oobsize for SAMSUNG K9GBG08U0A
	for (i = 0, n = 128; i < 640; i++, n++)
		buff[i] = n % 256;
	nfc_write_buf(mtd, buff, 1024);
	nfc_cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
	nfc_wait(mtd, nand);
	print_page(mtd, page);

	// test nfc read page
	nfc_read_page_hwecc(mtd, nand, buffer, page);
	printk(KERN_INFO "Test nfc_read_page_hwecc:\n");
	for (i = 0; i < 512; i++)
		printk("%02x ", buffer[i]);

	// erase block
	nfc_cmdfunc(mtd, NAND_CMD_ERASE1, 0, page);
	nfc_cmdfunc(mtd, NAND_CMD_ERASE2, -1, -1);
	nfc_wait(mtd, nand);
	print_page(mtd, page);

	// test nfc write page
	for (i = 0, n = 128; i < 8192; i++, n++)
		buffer[i] = n % 256;
	nfc_cmdfunc(mtd, NAND_CMD_SEQIN, 0, page);
	nfc_write_page_hwecc(mtd, nand, buffer);
	print_page(mtd, page);
}

// Test unit ops
static void test_ops(struct mtd_info *mtd)
{
	uint32_t page = 1280;
	uint32_t v1, v2;

	// test sequence read
	wait_cmdfifo_free();
	// NFC_DATA_TRANS = 1, NFC fetch data to RAM0
	// NFC_CMD_TYPE = 1,2,3 won't read out any thing
	v1 = NAND_CMD_READ0 | NFC_SEQ | NFC_SEND_CMD1 | NFC_DATA_TRANS | NFC_SEND_ADR | NFC_SEND_CMD2 | ((5 - 1) << 16) | NFC_WAIT_FLAG | (3 << 30);
	v2 = NAND_CMD_READSTART;
	writel(page << 16, NFC_REG_ADDR_LOW);
	writel(page >> 16, NFC_REG_ADDR_HIGH);
	//writel(2, NFC_REG_SECTOR_NUM);
	// NFC_REG_CNT = n, fetch n byte to RAM
	writel(1, NFC_REG_CNT);
	writel(v2, NFC_REG_RCMD_SET);
	writel(v1, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
	DBG_INFO("SEQ READ IO: %x %x %x %x %x %x\n", 
			 readb(NFC_REG_IO_DATA), 
			 readb(NFC_REG_IO_DATA), 
			 readb(NFC_REG_IO_DATA), 
			 readb(NFC_REG_IO_DATA), 
			 readb(NFC_REG_IO_DATA), 
			 readb(NFC_REG_IO_DATA));
	DBG_INFO("SEQ READ RAM: %x %x %x %x %x %x\n",
			 readb(NFC_RAM0_BASE),
			 readb(NFC_RAM0_BASE + 1),
			 readb(NFC_RAM0_BASE + 2),
			 readb(NFC_RAM0_BASE + 3),
			 readb(NFC_RAM0_BASE + 4),
			 readb(NFC_RAM0_BASE + 5));
}

int nfc_second_init(struct mtd_info *mtd)
{
	int i, err, j, n;
	uint32_t ctl;
	uint8_t id[8];
	struct nand_chip_param *chip_param = NULL;
	struct nand_chip *nand = mtd->priv;

	// get nand chip id
	nfc_cmdfunc(mtd, NAND_CMD_READID, 0, -1);
	for (i = 0; i < 8; i++)
		id[i] = nfc_read_byte(mtd);
	DBG_INFO("nand chip id: %x %x %x %x %x %x %x %x\n", 
			 id[0], id[1], id[2], id[3], 
			 id[4], id[5], id[6], id[7]);

	// find chip
	for (i = 0; nand_chip_param[i].id_len; i++) {
		int find = 1;
		for (j = 0; j < nand_chip_param[i].id_len; j++) {
			if (id[j] != nand_chip_param[i].id[j]) {
				find = 0;
				break;
			}
		}
		if (find) {
			chip_param = &nand_chip_param[i];
			DBG_INFO("find nand chip in sunxi database\n");
			break;
		}
	}

	// not find
	if (chip_param == NULL) {
		ERR_INFO("can't find nand chip in sunxi database\n");
		return -ENODEV;
	}

	// set final NFC clock freq
	if (chip_param->clock_freq > 30)
		chip_param->clock_freq = 30;
	sunxi_set_nand_clock(chip_param->clock_freq);
	DBG_INFO("set final clock freq to %dMHz\n", chip_param->clock_freq);

	// disable interrupt
	writel(0, NFC_REG_INT);
	// clear interrupt
	writel(readl(NFC_REG_ST), NFC_REG_ST);

	// set ECC mode
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	ctl &= ~NFC_ECC_MODE;
	ctl |= chip_param->ecc_mode << NFC_ECC_MODE_SHIFT;
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

	writel(0xff, NFC_REG_TIMING_CFG);
	writel(1 << nand->page_shift, NFC_REG_SPARE_AREA);

	// disable random
	disable_random();

	// setup ECC layout for ECC mode 3
	n = mtd->writesize / 1024 * 60;
	sunxi_ecclayout.eccbytes = n;
	for (i = 0; i < n; i++)
		sunxi_ecclayout.eccpos[i] = i;
	sunxi_ecclayout.oobavail = 0;
	sunxi_ecclayout.oobfree->offset = n;
	sunxi_ecclayout.oobfree->length = 0;
	nand->ecc.layout = &sunxi_ecclayout;
	nand->ecc.size = 1024;
	nand->ecc.bytes = 56;

	// setup DMA
	dma_hdle = dma_nand_request(1);
	if (dma_hdle == 0) {
		ERR_INFO("request DMA fail\n");
		err = -ENODEV;
		goto out;
	}

	// alloc buffer
	buffer_size = mtd->writesize + 1024;
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
	test_nfc(mtd);
	//test_ops(mtd);

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

