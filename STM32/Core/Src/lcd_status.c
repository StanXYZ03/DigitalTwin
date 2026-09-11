#include "lcd_status.h"
#include "bsp_lcd_rgb.h"
#include "fmc.h"
#include "ltdc.h"
#include <stdio.h>
#include <string.h>

#define LCD_PHYS_WIDTH             LCD_RGB_WIDTH
#define LCD_PHYS_HEIGHT            LCD_RGB_HEIGHT
#define LCD_LOGICAL_WIDTH          800U
#define LCD_LOGICAL_HEIGHT         480U
#define LCD_FRAMEBUFFER_ADDR       LCD_RGB_FB_ADDR
#define LCD_FRAMEBUFFER_BYTES      (LCD_PHYS_WIDTH * LCD_PHYS_HEIGHT * 2U)

#define LCD_COLOR_BACKGROUND       0x0861U
#define LCD_COLOR_PANEL            0x10C3U
#define LCD_COLOR_HEADER           0x0494U
#define LCD_COLOR_WHITE            0xFFFFU
#define LCD_COLOR_MUTED            0xBDF7U
#define LCD_COLOR_GREEN            0x07E0U
#define LCD_COLOR_YELLOW           0xFFE0U
#define LCD_COLOR_RED              0xF800U
#define LCD_COLOR_CYAN             0x07FFU

#define SDRAM_TIMEOUT              0xFFFFU
#define SDRAM_REFRESH_COUNT        371U
#define SDRAM_MODE_REGISTER        0x0230U

static uint8_t lcd_ready;

/* Debug-visible startup progress: 1 panel, 2 backlight, 3 SDRAM,
 * 4 LTDC layer, 5 framebuffer drawn/ready. */
volatile uint32_t g_lcd_status_stage;
volatile HAL_StatusTypeDef g_lcd_sdram_init_status;

void HAL_LTDC_ErrorCallback(LTDC_HandleTypeDef *hltdc_handle)
{
    m0_fmc_dbg.ltdc_error_callback_count++;
    m0_fmc_dbg.ltdc_error_code = hltdc_handle->ErrorCode;

    /* The HAL disables these sources after the first error.  Re-enable them
     * so the debugger counter reflects whether underruns continue after the
     * FMC bus-pacing fix. */
    __HAL_LTDC_ENABLE_IT(hltdc_handle, LTDC_IT_TE | LTDC_IT_FU);
}

static const uint8_t font_digits[10][7] =
{
    {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU},
    {0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU},
    {0x0EU, 0x11U, 0x01U, 0x02U, 0x04U, 0x08U, 0x1FU},
    {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU},
    {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x01U, 0x01U, 0x1EU},
    {0x0EU, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x0EU},
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U},
    {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU},
    {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x01U, 0x0EU}
};

static const uint8_t font_letters[26][7] =
{
    {0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU},
    {0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU},
    {0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU},
    {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U},
    {0x0EU, 0x11U, 0x10U, 0x17U, 0x11U, 0x11U, 0x0FU},
    {0x11U, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U},
    {0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU},
    {0x07U, 0x02U, 0x02U, 0x02U, 0x12U, 0x12U, 0x0CU},
    {0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U},
    {0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU},
    {0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U},
    {0x11U, 0x19U, 0x15U, 0x13U, 0x11U, 0x11U, 0x11U},
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U},
    {0x0EU, 0x11U, 0x11U, 0x11U, 0x15U, 0x12U, 0x0DU},
    {0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U},
    {0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU},
    {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U},
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU},
    {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x04U},
    {0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x15U, 0x0AU},
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x0AU, 0x11U, 0x11U},
    {0x11U, 0x11U, 0x0AU, 0x04U, 0x04U, 0x04U, 0x04U},
    {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x10U, 0x1FU}
};

static HAL_StatusTypeDef LCD_InitSDRAM(void)
{
    FMC_SDRAM_CommandTypeDef command;

    memset(&command, 0, sizeof(command));
    command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
    command.AutoRefreshNumber = 1U;

    command.CommandMode = FMC_SDRAM_CMD_CLK_ENABLE;
    if (HAL_SDRAM_SendCommand(&hsdram1, &command, SDRAM_TIMEOUT) != HAL_OK)
    {
        return HAL_ERROR;
    }
    HAL_Delay(1U);

    command.CommandMode = FMC_SDRAM_CMD_PALL;
    if (HAL_SDRAM_SendCommand(&hsdram1, &command, SDRAM_TIMEOUT) != HAL_OK)
    {
        return HAL_ERROR;
    }

    command.CommandMode = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
    command.AutoRefreshNumber = 8U;
    if (HAL_SDRAM_SendCommand(&hsdram1, &command, SDRAM_TIMEOUT) != HAL_OK)
    {
        return HAL_ERROR;
    }

    command.CommandMode = FMC_SDRAM_CMD_LOAD_MODE;
    command.AutoRefreshNumber = 1U;
    command.ModeRegisterDefinition = SDRAM_MODE_REGISTER;
    if (HAL_SDRAM_SendCommand(&hsdram1, &command, SDRAM_TIMEOUT) != HAL_OK)
    {
        return HAL_ERROR;
    }

    return HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT);
}

static void LCD_PutPixel(uint16_t x, uint16_t y, uint16_t color)
{
    uint16_t phys_x;
    uint16_t phys_y;
    volatile uint16_t *framebuffer = (volatile uint16_t *)LCD_FRAMEBUFFER_ADDR;

    if ((x >= LCD_LOGICAL_WIDTH) || (y >= LCD_LOGICAL_HEIGHT))
    {
        return;
    }

    phys_x = y;
    phys_y = (uint16_t)(LCD_LOGICAL_WIDTH - 1U - x);
    framebuffer[((uint32_t)phys_y * LCD_PHYS_WIDTH) + phys_x] = color;
}

static void LCD_FillRect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint16_t x;
    uint16_t y;

    if ((x1 >= LCD_LOGICAL_WIDTH) || (y1 >= LCD_LOGICAL_HEIGHT) || (x0 > x1) || (y0 > y1))
    {
        return;
    }

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            LCD_PutPixel(x, y, color);
        }
    }
}

static const uint8_t *LCD_GetGlyph(char character)
{
    static const uint8_t glyph_space[7] = {0U, 0U, 0U, 0U, 0U, 0U, 0U};
    static const uint8_t glyph_colon[7] = {0U, 0x04U, 0x04U, 0U, 0x04U, 0x04U, 0U};
    static const uint8_t glyph_dot[7] = {0U, 0U, 0U, 0U, 0U, 0x06U, 0x06U};
    static const uint8_t glyph_dash[7] = {0U, 0U, 0U, 0x1FU, 0U, 0U, 0U};

    if ((character >= '0') && (character <= '9'))
    {
        return font_digits[(uint8_t)(character - '0')];
    }
    if ((character >= 'A') && (character <= 'Z'))
    {
        return font_letters[(uint8_t)(character - 'A')];
    }
    if (character == ':')
    {
        return glyph_colon;
    }
    if (character == '.')
    {
        return glyph_dot;
    }
    if (character == '-')
    {
        return glyph_dash;
    }
    return glyph_space;
}

static void LCD_DrawCharacter(uint16_t x,
                              uint16_t y,
                              char character,
                              uint8_t scale,
                              uint16_t foreground,
                              uint16_t background)
{
    const uint8_t *glyph = LCD_GetGlyph(character);
    uint8_t row;
    uint8_t column;
    uint8_t sx;
    uint8_t sy;
    uint16_t color;

    LCD_FillRect(x,
                 y,
                 (uint16_t)(x + (6U * scale) - 1U),
                 (uint16_t)(y + (8U * scale) - 1U),
                 background);

    for (row = 0U; row < 7U; row++)
    {
        for (column = 0U; column < 5U; column++)
        {
            color = ((glyph[row] & (uint8_t)(1U << (4U - column))) != 0U) ? foreground : background;
            for (sy = 0U; sy < scale; sy++)
            {
                for (sx = 0U; sx < scale; sx++)
                {
                    LCD_PutPixel((uint16_t)(x + (column * scale) + sx),
                                 (uint16_t)(y + (row * scale) + sy),
                                 color);
                }
            }
        }
    }
}

static void LCD_DrawText(uint16_t x,
                         uint16_t y,
                         const char *text,
                         uint8_t scale,
                         uint16_t foreground,
                         uint16_t background)
{
    while ((*text != '\0') && (x < LCD_LOGICAL_WIDTH))
    {
        LCD_DrawCharacter(x, y, *text, scale, foreground, background);
        x = (uint16_t)(x + (6U * scale));
        text++;
    }
}

static void LCD_DrawChangedText(uint16_t x,
                                uint16_t y,
                                const char *text,
                                char *previous,
                                size_t previous_size,
                                uint8_t scale,
                                uint16_t foreground,
                                uint16_t background,
                                uint8_t force)
{
    size_t i;
    size_t new_length;
    size_t old_length;
    size_t draw_length;

    new_length = strlen(text);
    old_length = strlen(previous);
    if (new_length >= previous_size)
    {
        new_length = previous_size - 1U;
    }
    draw_length = (new_length > old_length) ? new_length : old_length;

    for (i = 0U; i < draw_length; ++i)
    {
        char new_character;

        new_character = (i < new_length) ? text[i] : ' ';
        if ((force != 0U) || (i >= old_length) ||
            (previous[i] != new_character))
        {
            LCD_DrawCharacter((uint16_t)(x + (i * 6U * scale)),
                              y,
                              new_character,
                              scale,
                              foreground,
                              background);
        }
    }

    memcpy(previous, text, new_length);
    previous[new_length] = '\0';
}

static void LCD_Flush(void)
{
    /* SDRAM framebuffer is MPU non-cacheable: order direct FMC writes before scanout. */
    __DSB();
}

static void LCD_DrawStaticPage(void)
{
    LCD_FillRect(0U, 0U, 799U, 479U, LCD_COLOR_BACKGROUND);
    LCD_FillRect(0U, 0U, 799U, 70U, LCD_COLOR_HEADER);
    LCD_FillRect(24U, 94U, 775U, 451U, LCD_COLOR_PANEL);

    LCD_DrawText(38U, 17U, "M0 ETHERNET DEMO", 5U, LCD_COLOR_WHITE, LCD_COLOR_HEADER);
    LCD_DrawText(50U, 112U, "PROGRAM  RUNNING", 4U, LCD_COLOR_GREEN, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 166U, "NETWORK  WAIT LINK", 4U, LCD_COLOR_YELLOW, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 220U, "PO COUNT  00000000", 4U, LCD_COLOR_CYAN, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 274U, "PIO       0000", 4U, LCD_COLOR_WHITE, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 328U, "PACKETS   00000000", 4U, LCD_COLOR_WHITE, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 382U, "ERRORS    00000000", 4U, LCD_COLOR_MUTED, LCD_COLOR_PANEL);
    LCD_DrawText(50U, 426U, "SERVER 192.168.100.100 5005", 3U, LCD_COLOR_MUTED, LCD_COLOR_PANEL);
}

HAL_StatusTypeDef LCD_Status_Init(void)
{
    g_lcd_status_stage = 1U;
    LCD_RGB_InitPanelOnly();

    /* Turn the backlight on before SDRAM access.  A later initialization
     * failure is therefore visible instead of looking like a dead LCD. */
    LCD_RGB_BacklightOn();
    g_lcd_status_stage = 2U;

    g_lcd_sdram_init_status = LCD_InitSDRAM();
    if (g_lcd_sdram_init_status != HAL_OK)
    {
        return g_lcd_sdram_init_status;
    }
    g_lcd_status_stage = 3U;

    LCD_RGB_Init();

    /* Keep the scanout path identical to the verified early-LCD project. */
    __HAL_LTDC_LAYER_ENABLE(&hltdc, LTDC_LAYER_1);
    if (LTDC_Layer1->CFBAR != LCD_FRAMEBUFFER_ADDR)
    {
        LTDC_Layer1->CFBAR = LCD_FRAMEBUFFER_ADDR;
    }
    LTDC->SRCR = LTDC_SRCR_IMR;
    g_lcd_status_stage = 4U;

    LCD_RGB_Fill(LCD_COLOR_BACKGROUND);
    LCD_DrawStaticPage();
    LCD_Flush();
    LCD_RGB_BacklightOn();

    lcd_ready = 1U;
    g_lcd_status_stage = 5U;
    return HAL_OK;
}

void LCD_Status_Update(const M0_DataSnapshot *snapshot,
                       LCD_StatusNetworkState network_state,
                       uint32_t send_count,
                       uint32_t error_count)
{
    char line[36];
    static char previous_network[36];
    static char previous_po[36];
    static char previous_pio[36];
    static char previous_packets[36];
    static char previous_errors[36];
    static uint16_t previous_network_color;
    static uint16_t previous_error_color;
    const char *network_text;
    uint16_t network_color;
    uint16_t error_color;

    if (lcd_ready == 0U)
    {
        return;
    }

    switch (network_state)
    {
        case LCD_STATUS_LINK_UP:
            network_text = "NETWORK  LINK UP";
            network_color = LCD_COLOR_CYAN;
            break;
        case LCD_STATUS_SEND_OK:
            network_text = "NETWORK  SEND OK";
            network_color = LCD_COLOR_GREEN;
            break;
        case LCD_STATUS_SEND_ERROR:
            network_text = "NETWORK  SEND ERROR";
            network_color = LCD_COLOR_RED;
            break;
        case LCD_STATUS_WAIT_LINK:
        default:
            network_text = "NETWORK  WAIT LINK";
            network_color = LCD_COLOR_YELLOW;
            break;
    }

    (void)snprintf(line, sizeof(line), "%-23s", network_text);
    LCD_DrawChangedText(50U, 166U, line, previous_network,
                        sizeof(previous_network), 4U, network_color,
                        LCD_COLOR_PANEL,
                        (uint8_t)(network_color != previous_network_color));
    previous_network_color = network_color;

    if (snapshot != NULL)
    {
        (void)snprintf(line, sizeof(line), "PO COUNT  %08lX", (unsigned long)snapshot->po);
        LCD_DrawChangedText(50U, 220U, line, previous_po,
                            sizeof(previous_po), 4U, LCD_COLOR_CYAN,
                            LCD_COLOR_PANEL, 0U);

        (void)snprintf(line, sizeof(line), "PIO       %04X", (unsigned int)snapshot->pio);
        LCD_DrawChangedText(50U, 274U, line, previous_pio,
                            sizeof(previous_pio), 4U, LCD_COLOR_WHITE,
                            LCD_COLOR_PANEL, 0U);
    }

    (void)snprintf(line, sizeof(line), "PACKETS   %08lX", (unsigned long)send_count);
    LCD_DrawChangedText(50U, 328U, line, previous_packets,
                        sizeof(previous_packets), 4U, LCD_COLOR_WHITE,
                        LCD_COLOR_PANEL, 0U);

    (void)snprintf(line, sizeof(line), "ERRORS    %08lX", (unsigned long)error_count);
    error_color = (error_count == 0U) ? LCD_COLOR_MUTED : LCD_COLOR_RED;
    LCD_DrawChangedText(50U, 382U, line, previous_errors,
                        sizeof(previous_errors), 4U, error_color,
                        LCD_COLOR_PANEL,
                        (uint8_t)(error_color != previous_error_color));
    previous_error_color = error_color;

    LCD_Flush();
}
