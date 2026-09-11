#include "main.h"
#include <string.h>
#include "bsp_lcd_rgb.h"
#include "ltdc.h"

typedef struct
{
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
    GPIO_TypeDef *sck_port;
    uint16_t sck_pin;
    GPIO_TypeDef *sda_port;
    uint16_t sda_pin;
} LCD_RGB_PanelPins;

typedef struct
{
    uint32_t hsw;
    uint32_t hbp;
    uint32_t hfp;
    uint32_t vsw;
    uint32_t vbp;
    uint32_t vfp;
    uint32_t pll3n;
    uint32_t pll3q;
    uint32_t pll3r;
} LCD_RGB_TimingTypeDef;

static LCD_RGB_PanelPins g_panel_pins;
static uint8_t s_panel_preinit_done = 0U;
static uint8_t s_panel_init_deferred = 0U;
static uint8_t s_backlight_gpio_ready = 0U;

static void LCD_RGB_DelayUS(uint32_t microseconds)
{
    uint32_t start;
    uint32_t ticks;

    /* Match the verified main project: use the Cortex-M7 cycle counter so
     * the panel's 3-wire command clock remains valid at every optimization. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    start = DWT->CYCCNT;
    ticks = microseconds * (SystemCoreClock / 1000000U);
    while ((uint32_t)(DWT->CYCCNT - start) < ticks)
    {
        __NOP();
    }
}

/* H743 board schematic candidates */
static const LCD_RGB_PanelPins s_panel_h743_mosi = {GPIOG, GPIO_PIN_10, GPIOH, GPIO_PIN_7, GPIOH, GPIO_PIN_8};
static const LCD_RGB_PanelPins s_panel_h743_miso = {GPIOG, GPIO_PIN_10, GPIOH, GPIO_PIN_7, GPIOG, GPIO_PIN_3};
/* New TK032F8004 adapter revision: CS moved from pin35 to pin39 (T_PEN -> PH6). */
static const LCD_RGB_PanelPins s_panel_h743_cs39_mosi = {GPIOH, GPIO_PIN_6, GPIOH, GPIO_PIN_7, GPIOH, GPIO_PIN_8};
static const LCD_RGB_PanelPins s_panel_h743_cs39_miso = {GPIOH, GPIO_PIN_6, GPIOH, GPIO_PIN_7, GPIOG, GPIO_PIN_3};

/* F429 document candidates */
static const LCD_RGB_PanelPins s_panel_f429_image = {GPIOI, GPIO_PIN_8, GPIOH, GPIO_PIN_6, GPIOI, GPIO_PIN_3};
static const LCD_RGB_PanelPins s_panel_f429_led = {GPIOH, GPIO_PIN_7, GPIOH, GPIO_PIN_6, GPIOI, GPIO_PIN_3};

static const LCD_RGB_PanelPins *LCD_RGB_GetPrimaryPanelPins(void)
{
#if (LCD_RGB_PANEL_PINSET == LCD_RGB_PANEL_PINSET_H743_SCHEMATIC)
    /* Verified v7_h743/TK032F8004 panel control path: */
    /* CS=PH6, SCK=PH7, SDA=PH8.  PG10 remains inactive/high. */
    return &s_panel_h743_cs39_mosi;
#elif (LCD_RGB_PANEL_PINSET == LCD_RGB_PANEL_PINSET_F429_IMAGE)
    return &s_panel_f429_image;
#else
#error "Invalid LCD_RGB_PANEL_PINSET"
#endif
}

static void LCD_RGB_GetTiming(LCD_RGB_TimingTypeDef *timing)
{
#if (LCD_RGB_TIMING_MODE == LCD_RGB_TIMING_800X480_LANDSCAPE)
    timing->hsw = 96U;
    timing->hbp = 10U;
    timing->hfp = 10U;
    timing->vsw = 2U;
    timing->vbp = 10U;
    timing->vfp = 10U;
    timing->pll3n = 48U;
    timing->pll3q = 5U;
    timing->pll3r = 10U;
#else
    /*
     * TK032F8004 (ILI9806E) 480x800 竖屏时序
     * DCLK: 16.6-41.7 MHz, Frame Rate: 54-66Hz
     *
     * 如果图像位置偏移，调整 HBP/HFP/VBP/VFP：
     * - 顶部被遮挡/有黑边：增大/减小 VBP
     * - 底部被遮挡/有黑边：增大/减小 VFP
     * - 左边被遮挡/有黑边：增大/减小 HBP
     * - 右边被遮挡/有黑边：增大/减小 HFP
     */
    timing->hsw = 1U;       /* 水平同步脉宽 */
    timing->hbp = 1U;       /* 水平后肩 */
    timing->hfp = 1U;       /* 水平前肩 */
    timing->vsw = 1U;       /* 垂直同步脉宽 */
    /* VBP：试过 8 更差，保持 2。现改 VFP 做对比（原 2→6；更差可试 4 或回到 2） */
    timing->vbp = 1U;       /* 垂直后肩 (VBP=0可能导致顶部异常) */
    timing->vfp = 1U;       /* 垂直前肩：底边/整帧垂直对齐常对 VFP 敏感 */
    timing->pll3n = 88U;    /* Proven board setting: PLL3 VCO = 440MHz */
    timing->pll3q = 4U;
    timing->pll3r = 18U;    /* Proven board setting: LTDC Clock = 24.4MHz */
#endif
}

static void LCD_RGB_EnableGPIOClock(GPIO_TypeDef *gpio)
{
    if (gpio == GPIOA)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    }
    else if (gpio == GPIOB)
    {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    }
    else if (gpio == GPIOC)
    {
        __HAL_RCC_GPIOC_CLK_ENABLE();
    }
    else if (gpio == GPIOD)
    {
        __HAL_RCC_GPIOD_CLK_ENABLE();
    }
    else if (gpio == GPIOE)
    {
        __HAL_RCC_GPIOE_CLK_ENABLE();
    }
    else if (gpio == GPIOF)
    {
        __HAL_RCC_GPIOF_CLK_ENABLE();
    }
    else if (gpio == GPIOG)
    {
        __HAL_RCC_GPIOG_CLK_ENABLE();
    }
    else if (gpio == GPIOH)
    {
        __HAL_RCC_GPIOH_CLK_ENABLE();
    }
    else if (gpio == GPIOI)
    {
        __HAL_RCC_GPIOI_CLK_ENABLE();
    }
    else if (gpio == GPIOJ)
    {
        __HAL_RCC_GPIOJ_CLK_ENABLE();
    }
    else if (gpio == GPIOK)
    {
        __HAL_RCC_GPIOK_CLK_ENABLE();
    }
}

static void LCD_RGB_InitOutputPin(GPIO_TypeDef *gpio, uint16_t pin)
{
    GPIO_InitTypeDef gpio_init;

    LCD_RGB_EnableGPIOClock(gpio);

    gpio_init.Pin = pin;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(gpio, &gpio_init);
}

static void LCD_RGB_PanelSetCS(GPIO_PinState state)
{
    HAL_GPIO_WritePin(g_panel_pins.cs_port, g_panel_pins.cs_pin, state);
}

static void LCD_RGB_PanelSetSCK(GPIO_PinState state)
{
    HAL_GPIO_WritePin(g_panel_pins.sck_port, g_panel_pins.sck_pin, state);
}

static void LCD_RGB_PanelSetSDA(GPIO_PinState state)
{
    HAL_GPIO_WritePin(g_panel_pins.sda_port, g_panel_pins.sda_pin, state);
}

static void LCD_RGB_SelectPanelPins(const LCD_RGB_PanelPins *pins)
{
    g_panel_pins = *pins;

    LCD_RGB_InitOutputPin(g_panel_pins.cs_port, g_panel_pins.cs_pin);
    LCD_RGB_InitOutputPin(g_panel_pins.sck_port, g_panel_pins.sck_pin);
    LCD_RGB_InitOutputPin(g_panel_pins.sda_port, g_panel_pins.sda_pin);

    LCD_RGB_PanelSetCS(GPIO_PIN_SET);
    LCD_RGB_PanelSetSCK(GPIO_PIN_SET);
    LCD_RGB_PanelSetSDA(GPIO_PIN_SET);
}

static void LCD_RGB_PanelWriteByte(uint8_t value)
{
    uint8_t i;

    for (i = 0U; i < 8U; i++)
    {
        LCD_RGB_PanelSetSCK(GPIO_PIN_RESET);
        LCD_RGB_PanelSetSDA((value & 0x80U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        LCD_RGB_DelayUS(1U);
        LCD_RGB_PanelSetSCK(GPIO_PIN_SET);
        LCD_RGB_DelayUS(1U);
        value <<= 1U;
    }
}

static void LCD_RGB_PanelWriteCmd(uint8_t cmd)
{
    LCD_RGB_PanelSetCS(GPIO_PIN_RESET);
    LCD_RGB_PanelWriteByte(0x70U);
    LCD_RGB_PanelWriteByte(cmd);
    LCD_RGB_PanelSetCS(GPIO_PIN_SET);
}

static void LCD_RGB_PanelWriteData(uint8_t data)
{
    LCD_RGB_PanelSetCS(GPIO_PIN_RESET);
    LCD_RGB_PanelWriteByte(0x72U);
    LCD_RGB_PanelWriteByte(data);
    LCD_RGB_PanelSetCS(GPIO_PIN_SET);
}

static void LCD_RGB_PanelWriteRegData(uint8_t reg, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    LCD_RGB_PanelWriteCmd(reg);
    for (i = 0U; i < len; i++)
    {
        LCD_RGB_PanelWriteData(data[i]);
    }
}

static void LCD_RGB_InitPanelRegisters(uint8_t pixel_format, uint8_t madctl)
{
    static const uint8_t reg_b0[] = {0x05U};           /* RGB Interface Mode: Enable DPI */
    static const uint8_t reg_b1[] = {0x00U, 0x14U, 0x06U};
    static const uint8_t reg_b2[] = {0x10U, 0xC8U};
    static const uint8_t reg_b3[] = {0x00U};
    static const uint8_t reg_b4[] = {0x04U};
    static const uint8_t reg_b5[] = {0x10U, 0x30U, 0x30U, 0x00U, 0x00U};
    static const uint8_t reg_b6[] = {0x01U, 0x18U, 0x02U, 0x40U, 0x10U, 0x00U};
    static const uint8_t reg_c0[] = {0x01U, 0x18U};
    static const uint8_t reg_c3[] = {0x03U, 0x04U, 0x03U, 0x03U, 0x03U};
    static const uint8_t reg_c4[] = {0x02U, 0x23U, 0x11U, 0x12U, 0x02U, 0x77U};
    static const uint8_t reg_c5[] = {0x73U};
    static const uint8_t reg_c6[] = {0x24U, 0x60U, 0x00U};
    static const uint8_t reg_d0[] = {0x14U, 0x01U, 0x53U, 0x25U, 0x02U, 0x02U, 0x66U, 0x14U, 0x03U};
    static const uint8_t reg_d1[] = {0x14U, 0x01U, 0x53U, 0x07U, 0x02U, 0x02U, 0x66U, 0x14U, 0x03U};

    LCD_RGB_PanelSetCS(GPIO_PIN_SET);
    HAL_Delay(20U);

    LCD_RGB_PanelWriteCmd(0x01U);  /* software reset */
    HAL_Delay(10U);

    LCD_RGB_PanelWriteCmd(0x11U);  /* Sleep Out */
    HAL_Delay(120U);

    /* 启用 RGB/DPI 接口模式 - 关键！ */
    LCD_RGB_PanelWriteRegData(0xB0U, reg_b0, sizeof(reg_b0));

    LCD_RGB_PanelWriteRegData(0xB1U, reg_b1, sizeof(reg_b1));
    LCD_RGB_PanelWriteRegData(0xB2U, reg_b2, sizeof(reg_b2));
    LCD_RGB_PanelWriteRegData(0xB3U, reg_b3, sizeof(reg_b3));
    LCD_RGB_PanelWriteRegData(0xB4U, reg_b4, sizeof(reg_b4));
    LCD_RGB_PanelWriteRegData(0xB5U, reg_b5, sizeof(reg_b5));
    LCD_RGB_PanelWriteRegData(0xB6U, reg_b6, sizeof(reg_b6));
    LCD_RGB_PanelWriteRegData(0xC0U, reg_c0, sizeof(reg_c0));
    LCD_RGB_PanelWriteRegData(0xC3U, reg_c3, sizeof(reg_c3));

    HAL_Delay(10U);

    LCD_RGB_PanelWriteRegData(0xC4U, reg_c4, sizeof(reg_c4));
    HAL_Delay(10U);

    LCD_RGB_PanelWriteRegData(0xC5U, reg_c5, sizeof(reg_c5));
    HAL_Delay(10U);

    LCD_RGB_PanelWriteRegData(0xC6U, reg_c6, sizeof(reg_c6));
    HAL_Delay(10U);

    LCD_RGB_PanelWriteRegData(0xD0U, reg_d0, sizeof(reg_d0));
    LCD_RGB_PanelWriteRegData(0xD1U, reg_d1, sizeof(reg_d1));
    LCD_RGB_PanelWriteRegData(0xD2U, reg_d0, sizeof(reg_d0));
    LCD_RGB_PanelWriteRegData(0xD3U, reg_d1, sizeof(reg_d1));
    LCD_RGB_PanelWriteRegData(0xD4U, reg_d0, sizeof(reg_d0));
    LCD_RGB_PanelWriteRegData(0xD5U, reg_d1, sizeof(reg_d1));

    /* 设置像素格式 (必须在 Display On 之前) */
    LCD_RGB_PanelWriteCmd(0x3AU);
    LCD_RGB_PanelWriteData(pixel_format);

    /* 设置 MADCTL (颜色顺序和扫描方向) */
    LCD_RGB_PanelWriteCmd(0x36U);
    LCD_RGB_PanelWriteData(madctl);

    HAL_Delay(10U);

    /* 最后开启显示 */
    LCD_RGB_PanelWriteCmd(0x29U);  /* Display On */
    HAL_Delay(20U);
}

static void LCD_RGB_InitPanelAutoProbe(void)
{
    const LCD_RGB_PanelPins *primary;
    uint8_t pixfmt_list[2];
    uint32_t pixfmt_count;
    uint32_t j;

#if (LCD_RGB_PANEL_AUTOPROBE == 1U)
    const LCD_RGB_PanelPins *profiles[] = {
        &s_panel_h743_cs39_mosi,
        &s_panel_h743_cs39_miso,
        &s_panel_h743_mosi,
        &s_panel_h743_miso,
        &s_panel_f429_image,
        &s_panel_f429_led
    };
    uint32_t i;
#endif

    primary = LCD_RGB_GetPrimaryPanelPins();

    pixfmt_list[0] = LCD_RGB_PANEL_PIXFMT;
    pixfmt_count = 1U;
#if (LCD_RGB_PANEL_AUTOPROBE == 1U)
    pixfmt_list[1] = (LCD_RGB_PANEL_PIXFMT == LCD_RGB_PANEL_PIXFMT_666) ? LCD_RGB_PANEL_PIXFMT_565 : LCD_RGB_PANEL_PIXFMT_666;
    pixfmt_count = 2U;
#endif

    LCD_RGB_SelectPanelPins(primary);
    for (j = 0U; j < pixfmt_count; j++)
    {
        LCD_RGB_InitPanelRegisters(pixfmt_list[j], LCD_RGB_PANEL_MADCTL);
    }

#if (LCD_RGB_PANEL_AUTOPROBE == 1U)
    for (i = 0U; i < (sizeof(profiles) / sizeof(profiles[0])); i++)
    {
        if (profiles[i] == primary)
        {
            continue;
        }

        LCD_RGB_SelectPanelPins(profiles[i]);
        for (j = 0U; j < pixfmt_count; j++)
        {
            LCD_RGB_InitPanelRegisters(pixfmt_list[j], LCD_RGB_PANEL_MADCTL);
        }
    }
#endif

    LCD_RGB_SelectPanelPins(primary);
}

static void LCD_RGB_SetLegacyPG10High(void)
{
    GPIO_InitTypeDef gpio_init;

    __HAL_RCC_GPIOG_CLK_ENABLE();
    gpio_init.Pin = GPIO_PIN_10;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &gpio_init);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_10, GPIO_PIN_SET);
}
static void LCD_RGB_InitBacklightGPIO(void)
{
    GPIO_InitTypeDef gpio_init;

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio_init.Pin = GPIO_PIN_14;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio_init);

    LCD_RGB_BacklightOff();
    s_backlight_gpio_ready = 1U;
}

static void LCD_RGB_InitClock(const LCD_RGB_TimingTypeDef *timing)
{
    RCC_PeriphCLKInitTypeDef periph_clk_init;

    memset(&periph_clk_init, 0, sizeof(periph_clk_init));
    periph_clk_init.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
    periph_clk_init.PLL3.PLL3M = 5U;
    periph_clk_init.PLL3.PLL3N = timing->pll3n;
    periph_clk_init.PLL3.PLL3P = 2U;
    periph_clk_init.PLL3.PLL3Q = timing->pll3q;
    periph_clk_init.PLL3.PLL3R = timing->pll3r;
    periph_clk_init.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
    periph_clk_init.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
    periph_clk_init.PLL3.PLL3FRACN = 0U;

    if (HAL_RCCEx_PeriphCLKConfig(&periph_clk_init) != HAL_OK)
    {
        Error_Handler();
    }
}

static void LCD_RGB_InitLTDCGPIO(void)
{
    GPIO_InitTypeDef gpio_init;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    gpio_init.Mode = GPIO_MODE_AF_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init.Alternate = GPIO_AF14_LTDC;

    gpio_init.Pin = GPIO_PIN_10;
    HAL_GPIO_Init(GPIOF, &gpio_init);  /* LCD_DE */

    gpio_init.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOG, &gpio_init);  /* LCD_R7, LCD_CLK */

    gpio_init.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
                    GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOH, &gpio_init);  /* LCD_R3~R6, LCD_G2~G4 */

    gpio_init.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
                    GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_9 |
                    GPIO_PIN_10;
    HAL_GPIO_Init(GPIOI, &gpio_init);  /* LCD_G5~G7, LCD_B4~B7, VSYNC, HSYNC */

    gpio_init.Pin = GPIO_PIN_8;
    HAL_GPIO_Init(GPIOA, &gpio_init);  /* LCD_B3 */
}

void LCD_RGB_InitPanelOnly(void)
{
    if (s_backlight_gpio_ready == 0U)
    {
        LCD_RGB_InitBacklightGPIO();
    }

    if (s_panel_init_deferred != 0U)
    {
        return;
    }

    if (s_panel_preinit_done == 0U)
    {
        LCD_RGB_InitPanelAutoProbe();
        LCD_RGB_SetLegacyPG10High();
        s_panel_preinit_done = 1U;
    }
}

void LCD_RGB_DeferPanelInit(void)
{
    /* PH7/PH8 also reach bridge/configuration logic.  Keep them quiet until
       the Artix-7 has completed its one-shot configuration sequence. */
    s_panel_init_deferred = 1U;
}

void LCD_RGB_ForcePanelInit(void)
{
    s_panel_init_deferred = 0U;
    LCD_RGB_InitPanelAutoProbe();
    LCD_RGB_SetLegacyPG10High();
    s_panel_preinit_done = 1U;
}

void LCD_RGB_Init(void)
{
    LTDC_LayerCfgTypeDef layer_cfg;

    LCD_RGB_InitPanelOnly();

    /* LTDC base timing, polarity and GPIO are owned by CubeMX (MX_LTDC_Init). */
    memset(&layer_cfg, 0, sizeof(layer_cfg));
    layer_cfg.WindowX0 = 0U;
    layer_cfg.WindowX1 = LCD_RGB_WIDTH;
    layer_cfg.WindowY0 = 0U;
    layer_cfg.WindowY1 = LCD_RGB_HEIGHT;
    layer_cfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB565;
    layer_cfg.FBStartAdress = LCD_RGB_FB_ADDR;
    layer_cfg.Alpha = 255U;
    layer_cfg.Alpha0 = 0U;
    layer_cfg.Backcolor.Red = 0U;
    layer_cfg.Backcolor.Green = 0U;
    layer_cfg.Backcolor.Blue = 0U;
    layer_cfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_CA;
    layer_cfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_CA;
    layer_cfg.ImageWidth = LCD_RGB_WIDTH;
    layer_cfg.ImageHeight = LCD_RGB_HEIGHT;

    if (HAL_LTDC_ConfigLayer(&hltdc, &layer_cfg, LTDC_LAYER_1) != HAL_OK)
    {
        Error_Handler();
    }

    /* CubeMX configures both LTDC layers by default.
     * This UI only uses layer 1; keep layer 2 disabled so it cannot fetch from
     * an unintended framebuffer and disturb scanout timing. */
    __HAL_LTDC_LAYER_DISABLE(&hltdc, LTDC_LAYER_2);
    LTDC_LAYER(&hltdc, LTDC_LAYER_2)->CFBAR = 0U;
    LTDC_LAYER(&hltdc, LTDC_LAYER_2)->CFBLNR = 0U;
    LTDC->SRCR = LTDC_SRCR_IMR;

    /* 启用 LTDC 全局中断 (关键！否则进不去 LineEventCallback 导致死机) */
    HAL_NVIC_SetPriority(LTDC_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(LTDC_IRQn);
}

void LCD_RGB_BacklightOn(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
}

void LCD_RGB_BacklightOff(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
}

void LCD_RGB_Fill(uint16_t color)
{
    uint32_t i;
    uint16_t *p;

    p = (uint16_t *)LCD_RGB_FB_ADDR;
    for (i = 0U; i < (LCD_RGB_WIDTH * LCD_RGB_HEIGHT); i++)
    {
        p[i] = color;
    }

    /* 关键：确保数据从Cache写入SDRAM，否则LTDC读取到的是未初始化的数据 */
    SCB_CleanDCache_by_Addr((uint32_t *)LCD_RGB_FB_ADDR, LCD_RGB_WIDTH * LCD_RGB_HEIGHT * 2);
}

