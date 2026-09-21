#include "fpga_autoconfig.h"

#include "bsp_i2c_ui.h"
#include "cmsis_os.h"

/* Bridge wiring, checked against the schematic and the working DSO project:
 *   PCAL6524 P1.5 -> U7 2A -> FPGA M0
 *   GND           -> U7 3A -> FPGA M1
 *   GND           -> U7 4A -> FPGA M2
 *   PH7 LOW enables U4..U7 (PH7/PH8 are swapped on the physical board).
 * U7 select is strapped LOW, so these A inputs produce M[2:0] = 001.
 */
#define PCAL6524_ADDR7          0x22U
#define PCAL6524_REG_INPUT1     0x01U
#define PCAL6524_REG_OUT1       0x05U
#define PCAL6524_REG_OUT0       0x04U
#define PCAL6524_REG_CFG0       0x0CU
#define PCAL6524_REG_CFG1       0x0DU
#define PCAL6524_REG_DRIVE0     0x40U
#define PCAL6524_REG_DRIVE1     0x41U
#define PCAL6524_REG_DRIVE2     0x42U
#define PCAL6524_REG_DRIVE3     0x43U
#define PCAL6524_REG_PULL_EN1   0x4DU
#define PCAL6524_REG_PULL_SEL1  0x51U
#define PCAL6524_REG_ODC        0x5CU
#define PCAL6524_P1_5           0x20U
#define PCAL6524_CFG1_M0_ONLY   0xDFU

#define MCP23017_PI_ADDR7       0x27U
#define MCP23017_REG_IODIRA     0x00U
#define MCP23017_REG_IOCON_B1   0x05U
#define MCP23017_REG_IOCON      0x0AU
#define MCP23017_REG_GPIOA      0x12U

#define FPGA_MUX_OE_PORT        GPIOH
#define FPGA_MUX_OE_PIN         GPIO_PIN_7
#define FPGA_PROGRAM_PORT       GPIOE
#define FPGA_PROGRAM_PIN        GPIO_PIN_3
#define FPGA_INIT_PORT          GPIOC
#define FPGA_INIT_PIN           GPIO_PIN_13
#define FPGA_DONE_PORT          GPIOC
#define FPGA_DONE_PIN           GPIO_PIN_14

#define FPGA_INIT_TIMEOUT_MS    500U
#define FPGA_DONE_TIMEOUT_MS    5000U
#define FPGA_RESET_ATTEMPTS     3U

volatile FPGA_AutoConfigDebug fpga_autoconfig_dbg;

static void FPGA_ModeControlPins_Init(void);

static void I2C_BusRecover(void)
{
  BSP_I2C_UI_BusRecover();
}

static int32_t PCAL_WriteReg(uint8_t reg, uint8_t value)
{
  return BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, reg, value);
}

static int32_t PCAL_ReadReg(uint8_t reg, uint8_t *value)
{
  return BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, reg, value);
}

static int32_t I2C_WriteBytes(uint8_t address7, uint8_t reg,
                              const uint8_t *data, uint32_t length)
{
  return BSP_I2C_UI_WriteBytes(address7, reg, data, length);
}

HAL_StatusTypeDef FPGA_PI_Init(void)
{
  const uint8_t bank0 = 0x00U;
  const uint8_t initial[2] = {0x00U, 0x00U};
  const uint8_t outputs[2] = {0x00U, 0x00U};

  fpga_autoconfig_dbg.pi_init_count++;
  if (fpga_autoconfig_dbg.pi_init_count == 1U)
  {
    fpga_autoconfig_dbg.pi_first_done =
        (HAL_GPIO_ReadPin(FPGA_DONE_PORT, FPGA_DONE_PIN) == GPIO_PIN_SET) ? 1U : 0U;
  }
  fpga_autoconfig_dbg.pi_ready = 0U;
  FPGA_ModeControlPins_Init();
  I2C_BusRecover();
  /* 0x05 is IOCON in BANK=1 and harmless GPINTENB in BANK=0. */
  if ((I2C_WriteBytes(MCP23017_PI_ADDR7, MCP23017_REG_IOCON_B1,
                      &bank0, 1U) != 0) ||
      (I2C_WriteBytes(MCP23017_PI_ADDR7, MCP23017_REG_IOCON,
                      &bank0, 1U) != 0) ||
      (I2C_WriteBytes(MCP23017_PI_ADDR7, MCP23017_REG_GPIOA,
                      initial, 2U) != 0) ||
      (I2C_WriteBytes(MCP23017_PI_ADDR7, MCP23017_REG_IODIRA,
                      outputs, 2U) != 0))
  {
    fpga_autoconfig_dbg.pi_i2c_result = -1;
    return HAL_ERROR;
  }
  fpga_autoconfig_dbg.pi_i2c_result = 0;
  fpga_autoconfig_dbg.pi_last_value = 0U;
  fpga_autoconfig_dbg.pi_ready = 1U;
  return HAL_OK;
}

HAL_StatusTypeDef FPGA_PI_Write(uint16_t value)
{
  const uint8_t data[2] = {
    (uint8_t)(value & 0x00FFU),
    (uint8_t)(value >> 8)
  };

  if (I2C_WriteBytes(MCP23017_PI_ADDR7, MCP23017_REG_GPIOA,
                     data, 2U) != 0)
  {
    fpga_autoconfig_dbg.pi_i2c_result = -1;
    fpga_autoconfig_dbg.pi_ready = 0U;
    return HAL_ERROR;
  }
  fpga_autoconfig_dbg.pi_i2c_result = 0;
  fpga_autoconfig_dbg.pi_last_value = value;
  fpga_autoconfig_dbg.pi_ready = 1U;
  return HAL_OK;
}

static void FPGA_ModeControlPins_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOH_CLK_ENABLE();
  BSP_I2C_UI_Init();

  HAL_GPIO_WritePin(FPGA_MUX_OE_PORT, FPGA_MUX_OE_PIN, GPIO_PIN_SET);
  gpio.Pin = FPGA_MUX_OE_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FPGA_MUX_OE_PORT, &gpio);
}

static void FPGA_AutoConfig_SamplePins(void)
{
  fpga_autoconfig_dbg.program_b =
      (HAL_GPIO_ReadPin(FPGA_PROGRAM_PORT, FPGA_PROGRAM_PIN) == GPIO_PIN_SET) ? 1U : 0U;
  fpga_autoconfig_dbg.init_b =
      (HAL_GPIO_ReadPin(FPGA_INIT_PORT, FPGA_INIT_PIN) == GPIO_PIN_SET) ? 1U : 0U;
  fpga_autoconfig_dbg.done =
      (HAL_GPIO_ReadPin(FPGA_DONE_PORT, FPGA_DONE_PIN) == GPIO_PIN_SET) ? 1U : 0U;
}

void FPGA_ExternalJtagRelease(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();

  /* PE2/TCK, PE6/TDI and PE5/TDO must not load or drive the cable. */
  gpio.Pin = GPIO_PIN_2 | GPIO_PIN_5 | GPIO_PIN_6;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &gpio);

  /* PE4 reaches TMS through BR_R45.  Use analog mode so even the STM32 input
     buffer and internal pull resistor are disconnected from external JTAG. */
  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &gpio);

  /* Retire the older PC6 push-pull TMS guard without adding another pull. */
  gpio.Pin = GPIO_PIN_6;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &gpio);

  FPGA_AutoConfig_SamplePins();
}

static HAL_StatusTypeDef FPGA_WaitPin(GPIO_TypeDef *port,
                                      uint16_t pin,
                                      GPIO_PinState state,
                                      uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  while (HAL_GPIO_ReadPin(port, pin) != state)
  {
    if ((HAL_GetTick() - start) >= timeout_ms)
    {
      return HAL_TIMEOUT;
    }
    osDelay(1U);
  }
  return HAL_OK;
}

static HAL_StatusTypeDef FPGA_SetModeAndEnable(void)
{
  uint8_t out1 = 0U;
  uint8_t cfg1 = 0U;
  uint8_t in1 = 0U;
  uint8_t mode_ready = 0U;
  uint32_t attempt;
  int32_t result;

  fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_SET_MODE;
  fpga_autoconfig_dbg.mux_enabled = 0U;
  FPGA_ModeControlPins_Init();

  /* Configure only P1.5.  The PCAL6524 data sheet requires output-stage mode
   * (5Ch) to be programmed before a configuration register changes a pin to
   * output.  Leave every unrelated expander pin at its reset input state. */
  result = -1;
  for (attempt = 0U; attempt < 10U; ++attempt)
  {
    I2C_BusRecover();
    result = PCAL_WriteReg(PCAL6524_REG_OUT1, 0xFFU);
    osDelay(1U);
    result |= PCAL_WriteReg(PCAL6524_REG_ODC, 0x00U);
    osDelay(1U);
    result |= PCAL_WriteReg(PCAL6524_REG_CFG1,
                            PCAL6524_CFG1_M0_ONLY);
    osDelay(2U);
    result |= PCAL_ReadReg(PCAL6524_REG_OUT1, &out1);
    result |= PCAL_ReadReg(PCAL6524_REG_CFG1, &cfg1);
    if ((result == 0) && ((out1 & PCAL6524_P1_5) != 0U) &&
        (cfg1 == PCAL6524_CFG1_M0_ONLY))
    {
      mode_ready = 1U;
      break;
    }

    /* Hardware fallback: if the direction latch remains at its reset input
     * value, bias P1.5 high with the PCAL's internal pull-up.  U7 presents
     * only a logic input load, so this is sufficient to select Master SPI. */
    if (result == 0)
    {
      result = PCAL_WriteReg(PCAL6524_REG_PULL_SEL1, PCAL6524_P1_5);
      result |= PCAL_WriteReg(PCAL6524_REG_PULL_EN1, PCAL6524_P1_5);
      osDelay(2U);
      result |= PCAL_ReadReg(PCAL6524_REG_INPUT1, &in1);
      if ((result == 0) && ((in1 & PCAL6524_P1_5) != 0U))
      {
        mode_ready = 1U;
        break;
      }
    }
    osDelay(10U);
  }
  fpga_autoconfig_dbg.i2c_result = result;
  fpga_autoconfig_dbg.pcal_out1 = out1;
  fpga_autoconfig_dbg.pcal_cfg1 = cfg1;

  if ((result != 0) || (mode_ready == 0U))
  {
    fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_I2C_ERROR;
    HAL_GPIO_WritePin(FPGA_MUX_OE_PORT, FPGA_MUX_OE_PIN, GPIO_PIN_SET);
    fpga_autoconfig_dbg.mux_enabled = 0U;
    FPGA_AutoConfig_SamplePins();
    return HAL_ERROR;
  }

  /* Keep the configuration-mode path actively driven.  With U7 disabled the
     Artix-7 mode pins have no guaranteed board-level straps. */
  HAL_GPIO_WritePin(FPGA_MUX_OE_PORT, FPGA_MUX_OE_PIN, GPIO_PIN_RESET);
  fpga_autoconfig_dbg.mux_enabled = 1U;
  return HAL_OK;
}

HAL_StatusTypeDef FPGA_ConfigModeHold_Run(void)
{
  fpga_autoconfig_dbg.run_count++;
  if (FPGA_SetModeAndEnable() != HAL_OK)
  {
    return HAL_ERROR;
  }

  fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_SUCCESS;
  FPGA_AutoConfig_SamplePins();
  return HAL_OK;
}

HAL_StatusTypeDef FPGA_AutoConfig_Run(void)
{
  uint32_t attempt;
  HAL_StatusTypeDef pi_result;

  fpga_autoconfig_dbg.run_count++;

  /* PI expansion is useful to establish a known key state, but it is not a
     configuration prerequisite.  In particular, a transient MCP23017 I2C
     failure must not prevent the independent PCAL6524 mode path from putting
     the FPGA into Master-SPI mode and restarting it from verified flash.
     M0_DataSource_Init() retries PI initialization during normal runtime. */
  FPGA_AutoConfig_SamplePins();
  pi_result = FPGA_PI_Init();

  /* The PCAL6524 mode path is the actual fail-safe boundary.  Never pulse
     PROGRAM_B unless M[2:0]=001 has been established and the mux enabled. */
  if (FPGA_SetModeAndEnable() != HAL_OK)
  {
    HAL_GPIO_WritePin(FPGA_PROGRAM_PORT, FPGA_PROGRAM_PIN, GPIO_PIN_SET);
    FPGA_AutoConfig_SamplePins();
    return HAL_ERROR;
  }

  /* Preserve the PI failure in its dedicated debug fields; configuration
     proceeds and the runtime retry path will recover the key expander. */
  (void)pi_result;

  /* Allow U7 and the mode nets to settle before PROGRAM_B is asserted. */
  osDelay(100U);
  fpga_autoconfig_dbg.config_attempts = 0U;
  fpga_autoconfig_dbg.init_low_seen = 0U;

  for (attempt = 0U; attempt < FPGA_RESET_ATTEMPTS; ++attempt)
  {
    fpga_autoconfig_dbg.config_attempts = attempt + 1U;
    fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_RESET;
    HAL_GPIO_WritePin(FPGA_PROGRAM_PORT, FPGA_PROGRAM_PIN, GPIO_PIN_RESET);

    /* INIT_B going low is the acknowledgement that PROGRAM_B actually
       reached the FPGA.  Previously this edge was never checked. */
    if (FPGA_WaitPin(FPGA_INIT_PORT, FPGA_INIT_PIN,
                     GPIO_PIN_RESET, FPGA_INIT_TIMEOUT_MS) != HAL_OK)
    {
      HAL_GPIO_WritePin(FPGA_PROGRAM_PORT, FPGA_PROGRAM_PIN, GPIO_PIN_SET);
      fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_INIT_TIMEOUT;
      osDelay(250U);
      continue;
    }
    fpga_autoconfig_dbg.init_low_seen++;
    osDelay(10U);
    HAL_GPIO_WritePin(FPGA_PROGRAM_PORT, FPGA_PROGRAM_PIN, GPIO_PIN_SET);

    fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_WAIT_INIT;
    if (FPGA_WaitPin(FPGA_INIT_PORT, FPGA_INIT_PIN,
                     GPIO_PIN_SET, FPGA_INIT_TIMEOUT_MS) != HAL_OK)
    {
      fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_INIT_TIMEOUT;
      osDelay(250U);
      continue;
    }

    fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_WAIT_DONE;
    if (FPGA_WaitPin(FPGA_DONE_PORT, FPGA_DONE_PIN,
                     GPIO_PIN_SET, FPGA_DONE_TIMEOUT_MS) == HAL_OK)
    {
      osDelay(100U);
      HAL_GPIO_WritePin(FPGA_MUX_OE_PORT, FPGA_MUX_OE_PIN, GPIO_PIN_RESET);
      fpga_autoconfig_dbg.mux_enabled = 1U;
      fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_SUCCESS;
      FPGA_AutoConfig_SamplePins();
      return HAL_OK;
    }

    fpga_autoconfig_dbg.state = FPGA_AUTOCONFIG_DONE_TIMEOUT;
    osDelay(250U);
  }

  /* Preserve the selected mode after a failure for hardware inspection. */
  HAL_GPIO_WritePin(FPGA_MUX_OE_PORT, FPGA_MUX_OE_PIN, GPIO_PIN_RESET);
  fpga_autoconfig_dbg.mux_enabled = 1U;
  FPGA_AutoConfig_SamplePins();
  return HAL_TIMEOUT;
}
