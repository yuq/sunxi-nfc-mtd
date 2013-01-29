#ifdef __LINUX__
#include <linux/io.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <plat/sys_config.h>
#else

#endif

#include "defs.h"
#include "regs.h"

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

	// open ahb nand clk
	cfg = readl(AHB_GATING_REG0);
	cfg |= 1 << AHB_GATING_NAND_CLK_SHIFT;
	writel(cfg, AHB_GATING_REG0);

	// set nand clock
	edo_clk = nand_max_clock * 2;

    cmu_clk = sunxi_get_pll5_clk();
	nand_clk_divid_ratio = cmu_clk / edo_clk;
	if (cmu_clk % edo_clk)
		nand_clk_divid_ratio++;
	if (nand_clk_divid_ratio){
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

// Set PIOC pin for NAND Flash use
static void sunxi_set_nand_pio(void)
{
#ifdef __LINUX__
	DBG_INFO("nand gpio_request\n");
    if (gpio_request_ex("nand_para", NULL) == 0)
		DBG_INFO("get nand pio ok \n");
#else
	writel(0x22222222, PC_CFG0_REG);
	writel(0x22222222, PC_CFG1_REG);
	writel(0x22222222, PC_CFG2_REG);
#endif
}

/////////////////////////////////////////////////////////////////
// Flash commands
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

static void set_addr(uint8_t *addr, int cnt)
{
	int i;
	uint32_t addr_low = 0;
	uint32_t addr_high = 0;

	for (i = 0; i < cnt; i++) {
		if (i < 4)
			addr_low |= (addr[i] << (i * 8));
		else
			addr_high |= (addr[i] << ((i - 4) * 8));
	}

	writel(addr_low, NFC_REG_ADDR_LOW);
	writel(addr_high, NFC_REG_ADDR_HIGH);
}

static void nand_cmd_reset(void)
{
	wait_cmdfifo_free();
	writel(NFC_SEND_CMD1 | 0xff, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
}

static void nand_cmd_read_id(void)
{
	wait_cmdfifo_free();
	set_addr(0, 0);
	writel(readl(NFC_REG_CTL) & ~NFC_RAM_METHOD, NFC_REG_CTL);
	writel(6, NFC_REG_CNT);
	writel(NFC_SEND_ADR | NFC_DATA_TRANS | NFC_SEND_CMD1 | 0x90, NFC_REG_CMD);
	wait_cmdfifo_free();
	wait_cmd_finish();
}

/////////////////////////////////////////////////////////////////
// NFC
//

static void nfc_select_chip(int chip)
{
	uint32_t ctl;
	// A10 has 8 CE pin to support 8 flash chips
    ctl = readl(NFC_REG_CTL);
    ctl &= ~NFC_CE_SEL;
	ctl |= ((chip & 7) << 24);
    writel(ctl, NFC_REG_CTL);
}

static void nfc_select_rb(int rb)
{
	uint32_t ctl;
	// A10 has 2 RB pin
	ctl = readl(NFC_REG_CTL);
	ctl &= ~NFC_RB_SEL;
	ctl |= ((rb & 0x1) << 3);
	writel(ctl, NFC_REG_CTL);
}

// 1 for ready, 0 for not ready
static inline int nfc_check_rb_ready(int rb)
{
	return (readl(NFC_REG_ST) & (NFC_RB_STATE0 << (rb & 0x3))) ? 1 : 0;
}

static void nfc_reset_chip(int chip)
{
	// select chip
	nfc_select_chip(chip);

    nand_cmd_reset();

	// wait rb0 ready
	nfc_select_rb(0);
	while (!nfc_check_rb_ready(0));

	// wait rb1 ready
	nfc_select_rb(1);
	while (!nfc_check_rb_ready(1));
}

static void nfc_get_chip_id(int chip, uint8_t *id)
{
	int i;

	// select chip
	nfc_select_chip(chip);

    nand_cmd_read_id();

	// get ID value
	for (i = 0; i < 6; i++)
		id[i] = readb(NFC_RAM0_BASE + i);
}

int nfc_init(struct mtd_info *info)
{
	uint32_t ctl;
	uint8_t id[6];

	// set NFC clock source
	sunxi_set_nand_clock(20);
	// set NFC pio
	sunxi_set_nand_pio();

	// disable interrupt
	writel(0, NFC_REG_INT);
	// clear interrupt
	writel(readl(NFC_REG_ST), NFC_REG_ST);

	// set ECC mode
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_ECC_MODE;
	writel(ctl, NFC_REG_ECC_CTL);

	// reset NFC
	ctl = readl(NFC_REG_CTL);
	ctl |= NFC_RESET;
	writel(ctl, NFC_REG_CTL);
	while(readl(NFC_REG_CTL) & NFC_RESET);

	ctl = NFC_EN;
	ctl |= ( (0 & 0x1) << 2); /* Bus width */
	ctl |= ( (0 & 0x1) << 6); /* CE_CTL */
	ctl |= ( (0 & 0x1) << 7); /* CE_CTL1 */
	ctl |= ( 0x1 << 8 );	  /* PAGE_SIZE = 2K */ /* Needs to be reset to actual page size */
	ctl |= ((0 & 0x3) << 18); /* DDR_TYPE */
	ctl |= ((0 & 0x1) << 31); /* DEBUG */
	writel(ctl, NFC_REG_CTL);

	ctl = (1 << 8); /* serial_access_mode = 1 */
	writel(ctl, NFC_REG_TIMING_CTL);
	writel(0xff, NFC_REG_TIMING_CFG);
	writel(0x800, NFC_REG_SPARE_AREA); /* Controller SRAM area where spare data should get accumulated during DMA data transfer */

	// disable random
	ctl = readl(NFC_REG_ECC_CTL);
	ctl &= ~NFC_RANDOM_EN;
	writel(ctl, NFC_REG_ECC_CTL);

	// reset NAND flash chip
	nfc_reset_chip(0);

	// get Flash chip ID
	nfc_get_chip_id(0, id);
	DBG_INFO("flash chip id= %x %x %x %x %x %x\n", id[0], id[1], id[2], id[3], id[4], id[5]);

	return 0;
}

void nfc_chip_init(struct mtd_info *info)
{

}

void nfc_exit(struct mtd_info *info)
{

}

