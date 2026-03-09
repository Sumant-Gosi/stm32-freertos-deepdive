/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body — DMA + UART Demo
  *
  * Key changes from previous branches:
  * - HAL_UART_Transmit()        → HAL_UART_Transmit_DMA() (non-blocking)
  * - osSemaphoreRelease()       → xSemaphoreGiveFromISR() in callback
  *   Reason: CMSIS wrapper not reliably ISR-safe on all STM32 HAL versions.
  *           Native FreeRTOS xSemaphoreGiveFromISR() is explicitly ISR-safe.
  * - portYIELD_FROM_ISR() ensures waiting task wakes immediately after ISR
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "cmsis_os.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"           /* CHANGED: needed for xSemaphoreCreateBinary */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart2_tx;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for ButtonTask */
osThreadId_t ButtonTaskHandle;
const osThreadAttr_t ButtonTask_attributes = {
  .name = "ButtonTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for ConsumerTask */
osThreadId_t ConsumerTaskHandle;
const osThreadAttr_t ConsumerTask_attributes = {
  .name = "ConsumerTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

/* Software Timer */
osTimerId_t LedBlinkTimerHandle;
const osTimerAttr_t LedBlinkTimer_attributes = {
  .name = "LedBlinkTimer"
};

/* ------------------------------------------------------------------ */
/*  CHANGED: Native FreeRTOS semaphore handle                         */
/*  Using SemaphoreHandle_t instead of osSemaphoreId_t               */
/*  Reason: xSemaphoreGiveFromISR() requires native FreeRTOS handle  */
/* ------------------------------------------------------------------ */
SemaphoreHandle_t UartDmaSemHandle;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_DMA_Init(void);
void UART_Print(const char *msg);
void StartDefaultTask(void *argument);
void LedBlinkCallback(void *argument);
void ButtonTask2(void *argument);
void ConsumerTask3(void *argument);

/* -------------------------------------------------------------------------- */
/*  CHANGED: Uses xSemaphoreTake — native FreeRTOS API                       */
/*  Acquires semaphore, starts DMA, returns immediately                       */
/*  CPU is free the moment HAL_UART_Transmit_DMA() returns                   */
/* -------------------------------------------------------------------------- */
void UART_Print(const char *msg)
{
    xSemaphoreTake(UartDmaSemHandle, portMAX_DELAY);
    HAL_UART_Transmit_DMA(&huart2, (uint8_t*)msg, strlen(msg));
}

/* -------------------------------------------------------------------------- */
/*  CHANGED: xSemaphoreGiveFromISR — explicitly ISR-safe                     */
/*  Called automatically by HAL when DMA finishes all bytes                  */
/*  portYIELD_FROM_ISR: if a task was waiting, wake it immediately           */
/* -------------------------------------------------------------------------- */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance == USART2)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(UartDmaSemHandle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* -------------------------------------------------------------------------- */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  /* DMA must be initialized BEFORE UART */
  MX_DMA_Init();
  MX_GPIO_Init();
  MX_USART2_UART_Init();

  osKernelInitialize();

  /* ------------------------------------------------------------------ */
  /*  CHANGED: xSemaphoreCreateBinary + xSemaphoreGive                  */
  /*  Binary semaphore starts empty after creation                       */
  /*  xSemaphoreGive() makes it available immediately for first print   */
  /* ------------------------------------------------------------------ */
  UartDmaSemHandle = xSemaphoreCreateBinary();
  xSemaphoreGive(UartDmaSemHandle);

  LedBlinkTimerHandle = osTimerNew(LedBlinkCallback, osTimerPeriodic, NULL, &LedBlinkTimer_attributes);

  defaultTaskHandle  = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  ButtonTaskHandle   = osThreadNew(ButtonTask2,      NULL, &ButtonTask_attributes);
  ConsumerTaskHandle = osThreadNew(ConsumerTask3,    NULL, &ConsumerTask_attributes);

  osTimerStart(LedBlinkTimerHandle, 500);

  osKernelStart();

  while (1) {}
}

/* -------------------------------------------------------------------------- */
void StartDefaultTask(void *argument)
{
  for(;;) { osDelay(1); }
}

/* -------------------------------------------------------------------------- */
void LedBlinkCallback(void *argument)
{
  HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
  UART_Print("[LedBlink] LED toggled\r\n");
}

/* -------------------------------------------------------------------------- */
void ButtonTask2(void *argument)
{
  uint8_t lastState = GPIO_PIN_SET;

  for(;;)
  {
    uint8_t currentState = HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin);

    if(currentState == GPIO_PIN_RESET && lastState == GPIO_PIN_SET)
    {
      xTaskNotifyGive((TaskHandle_t)ConsumerTaskHandle);
      UART_Print("[Button] Pressed! Notification sent\r\n");
      osDelay(200);
    }

    lastState = currentState;
    osDelay(50);
  }
}

/* -------------------------------------------------------------------------- */
void ConsumerTask3(void *argument)
{
  for(;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    UART_Print("[Consumer] Notification received! Button was pressed\r\n");
  }
}

/* -------------------------------------------------------------------------- */
static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);

  HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/* -------------------------------------------------------------------------- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

  if (HAL_PWREx_EnableOverDrive() != HAL_OK) { Error_Handler(); }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                               | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) { Error_Handler(); }
}

/* -------------------------------------------------------------------------- */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK) { Error_Handler(); }
}

/* -------------------------------------------------------------------------- */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);
}

/* -------------------------------------------------------------------------- */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM1) { HAL_IncTick(); }
}

/* -------------------------------------------------------------------------- */
void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
