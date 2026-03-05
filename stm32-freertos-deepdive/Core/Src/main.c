/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body — Priority Inversion Demo
  *
  * PHASE 1 (BUG):  SharedLock is a binary semaphore — no priority inheritance
  *                 MedTask runs freely while HighTask is stuck. Bug visible.
  *
  * PHASE 2 (FIX):  Change SharedLock to a mutex — priority inheritance kicks in
  *                 LowTask gets promoted, finishes fast, HighTask unblocks.
  *
  * To switch phases: comment/uncomment the two lines marked PHASE 1 / PHASE 2
  * in main() where SharedLockHandle is created.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "cmsis_os.h"
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* ------------------------------------------------------------------ */
/*  Software Timer — LED blink, shows system is still alive           */
/* ------------------------------------------------------------------ */
osTimerId_t LedBlinkTimerHandle;
const osTimerAttr_t LedBlinkTimer_attributes = {
  .name = "LedBlinkTimer"
};

/* ------------------------------------------------------------------ */
/*  Priority Inversion Demo Tasks                                      */
/* ------------------------------------------------------------------ */
osThreadId_t HighTaskHandle;
const osThreadAttr_t HighTask_attributes = {
  .name = "HighTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh,       /* highest */
};

osThreadId_t MedTaskHandle;
const osThreadAttr_t MedTask_attributes = {
  .name = "MedTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,     /* medium */
};

osThreadId_t LowTaskHandle;
const osThreadAttr_t LowTask_attributes = {
  .name = "LowTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,        /* lowest */
};

/* ------------------------------------------------------------------ */
/*  SharedLock — swap between semaphore and mutex to see the effect   */
/* ------------------------------------------------------------------ */
osSemaphoreId_t SharedLockHandle;         /* PHASE 1: binary semaphore */
osMutexId_t     SharedMutexHandle;        /* PHASE 2: mutex             */

const osSemaphoreAttr_t SharedLock_attributes = { .name = "SharedLock" };
const osMutexAttr_t     SharedMutex_attributes = { .name = "SharedMutex" };

/* UartMutex — still protecting UART prints */
osMutexId_t UartMutexHandle;
const osMutexAttr_t UartMutex_attributes = { .name = "UartMutex" };

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
void UART_Print(const char *msg);
void StartDefaultTask(void *argument);
void LedBlinkCallback(void *argument);
void HighTaskFunc(void *argument);
void MedTaskFunc(void *argument);
void LowTaskFunc(void *argument);

/* -------------------------------------------------------------------------- */
void UART_Print(const char *msg)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
}

/* -------------------------------------------------------------------------- */
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USART2_UART_Init();

  osKernelInitialize();

  /* UART Mutex */
  UartMutexHandle = osMutexNew(&UartMutex_attributes);

  /* ------------------------------------------------------------------ */
  /*  PHASE 1 (BUG): Binary semaphore — no priority inheritance         */
  /*  Comment this out and uncomment PHASE 2 to see the fix             */
  /* ------------------------------------------------------------------ */
  //SharedLockHandle = osSemaphoreNew(1, 1, &SharedLock_attributes);

  /* ------------------------------------------------------------------ */
  /*  PHASE 2 (FIX): Mutex — priority inheritance enabled               */
  /*  Uncomment this and comment out PHASE 1 to see the fix             */
  /* ------------------------------------------------------------------ */
  SharedMutexHandle = osMutexNew(&SharedMutex_attributes);

  /* Software Timer — 1000ms so it doesn't flood the output */
  LedBlinkTimerHandle = osTimerNew(LedBlinkCallback, osTimerPeriodic, NULL, &LedBlinkTimer_attributes);

  /* Create Tasks */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  HighTaskHandle    = osThreadNew(HighTaskFunc,     NULL, &HighTask_attributes);
  MedTaskHandle     = osThreadNew(MedTaskFunc,      NULL, &MedTask_attributes);
  LowTaskHandle     = osThreadNew(LowTaskFunc,      NULL, &LowTask_attributes);

  osTimerStart(LedBlinkTimerHandle, 1000);

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

  osMutexAcquire(UartMutexHandle, osWaitForever);
  UART_Print("[Timer] LED toggled — system alive\r\n");
  osMutexRelease(UartMutexHandle);
}

/* -------------------------------------------------------------------------- */
/*  LowTask: acquires shared lock, busy-waits, releases                       */
/*  Busy-wait = does NOT yield to scheduler — this is intentional             */
/*  osDelay would hide the bug by yielding, busy-wait keeps the lock held     */
/* -------------------------------------------------------------------------- */
void LowTaskFunc(void *argument)
{
  osDelay(100); /* let HighTask and MedTask start first */

  for(;;)
  {
    /* PHASE 1 */ osSemaphoreAcquire(SharedLockHandle, osWaitForever);
    /* PHASE 2 */ /* osMutexAcquire(SharedMutexHandle, osWaitForever); */

    osMutexAcquire(UartMutexHandle, osWaitForever);
    UART_Print("[LowTask]  Acquired lock. Doing slow work...\r\n");
    osMutexRelease(UartMutexHandle);

    /* Busy-wait: simulate slow work WITHOUT yielding */
    /* This keeps the lock held so HighTask stays blocked */
    volatile uint32_t i;
    for(i = 0; i < 2000000; i++) { __NOP(); }

    osMutexAcquire(UartMutexHandle, osWaitForever);
    UART_Print("[LowTask]  Released lock.\r\n");
    osMutexRelease(UartMutexHandle);

    /* PHASE 1 */ //osSemaphoreRelease(SharedLockHandle);
    /* PHASE 2 */ osMutexRelease(SharedMutexHandle);

    osDelay(500);
  }
}

/* -------------------------------------------------------------------------- */
/*  MedTask: no lock needed — just runs freely                                */
/*  In PHASE 1 you will see this printing while HighTask is stuck             */
/* -------------------------------------------------------------------------- */
void MedTaskFunc(void *argument)
{
  for(;;)
  {
    osMutexAcquire(UartMutexHandle, osWaitForever);
    UART_Print("[MedTask]  Running freely...\r\n");
    osMutexRelease(UartMutexHandle);

    osDelay(300);
  }
}

/* -------------------------------------------------------------------------- */
/*  HighTask: highest priority — should run first always                      */
/*  But gets blocked waiting for SharedLock held by LowTask                   */
/*  In PHASE 1: stuck while MedTask runs (priority inversion bug)             */
/*  In PHASE 2: LowTask gets promoted, finishes fast, HighTask unblocks       */
/* -------------------------------------------------------------------------- */
void HighTaskFunc(void *argument)
{
  osDelay(200); /* let LowTask acquire the lock first */

  for(;;)
  {
    osMutexAcquire(UartMutexHandle, osWaitForever);
    UART_Print("[HighTask] Waiting for lock...\r\n");
    osMutexRelease(UartMutexHandle);

    /* PHASE 1 */ osSemaphoreAcquire(SharedLockHandle, osWaitForever);
    /* PHASE 2 */ /* osMutexAcquire(SharedMutexHandle, osWaitForever); */

    osMutexAcquire(UartMutexHandle, osWaitForever);
    UART_Print("[HighTask] Got lock! Running critical work.\r\n");
    osMutexRelease(UartMutexHandle);

    /* PHASE 1 */ //osSemaphoreRelease(SharedLockHandle);
    /* PHASE 2 */ osMutexRelease(SharedMutexHandle);

    osDelay(500);
  }
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
