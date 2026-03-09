# stm32-freertos-deepdive

A hands-on journey through FreeRTOS on an STM32 NUCLEO-F446RE — from theory to working hardware.

---

## Why I Built This

I had heard about RTOS concepts in theory — pre-emptive scheduling, mutexes, race conditions — but theory alone never fully landed. I wanted to see these things happen in real hardware, with real output on a serial monitor, where I could break things deliberately and watch them get fixed.

This project is the result. It is not a polished product. It is a structured learning exercise, built one concept at a time, where each branch of the repository represents one idea taken from theory to working code.

---

## Hardware & Tools

| | |
|---|---|
| **Board** | STMicroelectronics NUCLEO-F446RE (ARM Cortex-M4) |
| **RTOS** | FreeRTOS via CMSIS-RTOS v2 API |
| **Toolchain** | STM32CubeIDE |
| **Language** | C (GNU11) |
| **Serial Monitor** | screen / any 115200 baud terminal |

---

## Repository Structure

Each concept lives in its own branch, built on top of the previous one:

```
main                        ← foundation: multitasking + queue + mutex
feature/semaphores          ← replace queue with binary semaphore
feature/task-notifications  ← replace semaphore with direct task notification
feature/software-timers     ← replace LED task with a software timer
feature/priority-inversion  ← demonstrate the bug, then fix it with mutex
feature/dma-uart            ← non-blocking UART via DMA
```

Reading the branches in order tells the full story.

---

## The Journey

### Starting Point — Bare Metal vs RTOS

Before this project, my embedded experience was bare-metal C. A typical loop looked like this:

```c
while(1) {
    HAL_GPIO_TogglePin(...);
    HAL_Delay(500);         // blocks everything
}
```

`HAL_Delay()` freezes the entire CPU. Nothing else can run. This works for one task but falls apart the moment you need two things happening at the same time.

The RTOS equivalent is `osDelay()`. The difference is fundamental — `osDelay()` does not freeze the CPU. It tells the scheduler: *"I'm done for now, put me to sleep and run something else. Wake me in 500ms."* The CPU is free the entire time I'm sleeping.

Seeing two LEDs blink at different rates simultaneously — each in its own task, each using `osDelay()` — was the first proof that the scheduler was actually working.

---

### Chapter 1 — Multitasking, Queue, and the Mutex (`main` branch)

The foundation of the project has three tasks running concurrently:

**LedBlinkTask** toggles an LED every 500ms. Simple and periodic.

**ButtonTask** polls the user button. When pressed, it sends a message to a queue. This is the producer.

**ConsumerTask** blocks on the queue waiting for a message. When one arrives, it wakes up, reacts, and goes back to sleep. This is the consumer. It uses zero CPU while waiting.

**The race condition problem** appeared naturally. Both tasks were printing to UART. The output looked like this:

```
Task 1: LED ToggTlaesdk 2: LED Toggled
```

Both tasks tried to write to the UART hardware at the same time and corrupted each other's data. This is a race condition — a classic concurrency bug.

The fix was a **mutex** (mutual exclusion lock). Only one task can hold the mutex at a time. The other task blocks until the first releases it. After wrapping every `UART_Print()` call in `osMutexAcquire()` and `osMutexRelease()`, the output became perfectly clean.

<img width="491" height="715" alt="Screenshot 2026-03-05 at 09 21 30" src="https://github.com/user-attachments/assets/688a4974-d04e-4dad-8e18-a13250741927" />


**Key insight:** The queue controls *when* tasks communicate. The mutex controls *safe access* to shared hardware. They solve different problems and are used together.

---

### Chapter 2 — Semaphores (`feature/semaphores`)

The queue in Chapter 1 carried data — a `uint16_t` message from ButtonTask to ConsumerTask. But what if you don't need to send data? What if you just need to say *"something happened, go"*?

That's a **binary semaphore**. It's a counter that lives between two tasks:

- Starts at 0 — ConsumerTask tries to acquire it, finds nothing, sleeps
- Button pressed — ButtonTask releases it, counter becomes 1
- RTOS wakes ConsumerTask — it acquires the semaphore, counter drops to 0, runs, sleeps again

No data. Just a signal. Simpler than a queue when data is not needed.

```
Queue:     ButtonTask → [carries data] → ConsumerTask
Semaphore: ButtonTask → [signal only]  → ConsumerTask
```

The output is identical to Chapter 1. That's the point — same behavior, simpler mechanism.

<img width="475" height="552" alt="Screenshot 2026-03-05 at 09 41 16" src="https://github.com/user-attachments/assets/b0adc477-3500-45a6-b77a-04a3e62f24ae" />

---

### Chapter 3 — Task Notifications (`feature/task-notifications`)

A semaphore is a separate object that sits between two tasks. A **task notification** removes even that.

Every FreeRTOS task already has a tiny built-in notification counter. Instead of creating a semaphore object, ButtonTask knocks directly on ConsumerTask's door:

```c
// ButtonTask — knocks directly on ConsumerTask
xTaskNotifyGive(ConsumerTaskHandle);

// ConsumerTask — waits for a knock
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
```

No object to create. No handle to manage. Less RAM, faster, and the intent reads clearly in code — one task gives, one task takes.

<img width="513" height="687" alt="Screenshot 2026-03-05 at 10 54 14" src="https://github.com/user-attachments/assets/0cdfd424-54ff-4d7a-8c1a-cf20477bca75" />


The limitation: task notifications are point-to-point. One sender, one specific receiver. Semaphores can signal any waiting task. For this use case — button to consumer — notifications are the right tool.

---

### Chapter 4 — Software Timers (`feature/software-timers`)

LedBlinkTask existed solely to toggle an LED every 500ms. It had its own stack, its own memory, and spent almost all of its time sleeping in `osDelay()`. That is wasteful for something so simple.

A **software timer** replaces the entire task with a small callback function:

```c
void LedBlinkCallback(void *argument)
{
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    UART_Print("[LedBlink] LED toggled by timer\r\n");
}
```

The timer fires this function every 500ms automatically. No dedicated task. No permanent stack. No sleeping task consuming memory.

**One important rule:** timer callbacks must be short and fast. No `osDelay()`, no blocking calls inside a callback. If the logic is complex, use a task. If it's just "do X every N ms", use a timer.

---

### Chapter 5 — Priority Inversion (`feature/priority-inversion`)

This was the concept that clicked the most — because it is a real bug that caused a real spacecraft to malfunction.

**The theory:**

In an RTOS, higher priority tasks always run first. Always. That is the entire point of priority scheduling.

Priority inversion breaks this guarantee. Here is how:

```
LowTask  acquires a lock
HighTask wakes up, needs the same lock → blocked
MedTask  wakes up, needs no lock → runs freely
```

HighTask has the highest priority but is stuck waiting. MedTask has lower priority but runs freely. **The priorities are inverted.** A medium priority task is effectively more important than a high priority task.

**Seeing it on hardware:**

Phase 1 used a binary semaphore as the shared lock. The serial output showed it clearly:

```
[LowTask]  Acquired lock. Doing slow work...
[HighTask] Waiting for lock...
[MedTask]  Running freely...        ← should not be running
[MedTask]  Running freely...        ← HighTask is stuck
[LowTask]  Released lock.
[HighTask] Got lock! Running critical work.
```

MedTask printing repeatedly while HighTask waited — visible, real, exactly as described in lectures.

**The fix — Priority Inheritance:**

Swapping the semaphore for a mutex fixed it. When HighTask blocked waiting for the mutex held by LowTask, FreeRTOS automatically promoted LowTask to HighTask's priority temporarily. LowTask finished its slow work quickly, released the mutex, dropped back to low priority. HighTask ran immediately.

Phase 2 output:

```
[LowTask]  Acquired lock. Doing slow work...
[HighTask] Waiting for lock...
[LowTask]  Released lock.
[HighTask] Got lock! Running critical work.  ← runs immediately
[MedTask]  Running freely...                 ← correct order restored
```

<img width="515" height="780" alt="Screenshot 2026-03-05 at 11 40 39" src="https://github.com/user-attachments/assets/c417d136-1de5-4199-8a7e-ce827899667a" />


**Why a semaphore causes the bug but a mutex fixes it:**

Semaphores have no concept of ownership. No task "owns" a semaphore — anyone can release it. Because of this, a semaphore cannot implement priority inheritance. It has no way to know which task is holding it.

A mutex tracks ownership. FreeRTOS knows exactly which task holds it. That ownership is what enables priority inheritance — the scheduler can promote the right task because it knows who to promote.

**Historical note:** This exact bug caused the Mars Pathfinder rover to reset repeatedly in 1997. A high priority task kept getting blocked by a low priority task holding a mutex, causing a watchdog timer to reset the entire system. The JPL engineers diagnosed and fixed it remotely — from Earth, on Mars.

---

### Chapter 6 — DMA + UART (`feature/dma-uart`)

Every previous chapter used `HAL_UART_Transmit()` — a blocking call. While a task was printing, the CPU sat idle waiting for every single byte to be sent over the wire. At 115200 baud, transmitting 30 bytes takes roughly 2.6ms. That is 2.6ms of wasted CPU time per print call.

DMA (Direct Memory Access) fixes this. Instead of the CPU carrying each byte to the UART register manually, the CPU hands the entire buffer to the DMA controller and returns immediately. DMA sends the bytes in the background using dedicated hardware. The CPU is free the entire time.

```
WITHOUT DMA:
CPU: [send byte 1]...[send byte 2]...[send byte 3]...[done] → continue

WITH DMA:
CPU: [hand buffer to DMA] → continues immediately
DMA:                      [byte1][byte2][byte3][done] → fires interrupt
```

**The challenge — when to release the lock:**

In previous chapters the mutex was released immediately after `UART_Print()` returned. With DMA that is wrong — the function returns before transmission is actually finished. Releasing the mutex too early means another task can start a new DMA transfer while the old one is still running. The result is corrupted output.

The solution: release the lock only when DMA signals it is done. DMA fires an interrupt on completion, which calls `HAL_UART_TxCpltCallback()` automatically. That is the correct place to release.

```c
void UART_Print(const char *msg)
{
    xSemaphoreTake(UartDmaSemHandle, portMAX_DELAY);
    HAL_UART_Transmit_DMA(&huart2, (uint8_t*)msg, strlen(msg));
    // do NOT release here — DMA still transmitting
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    // DMA finished — safe to release now
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(UartDmaSemHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
```
<img width="491" height="715" alt="Screenshot 2026-03-05 at 09 21 30" src="https://github.com/user-attachments/assets/177837ed-1c69-4369-88ae-9d6bbd914431" />

**The debugging journey — three failures before it worked:**

*Attempt 1 — mutex in callback:* Used `osMutexRelease()` inside the callback. Output went silent after the first print. A mutex cannot be released from ISR context in FreeRTOS — it has ownership, the ISR has no task identity, the release is silently rejected.

*Attempt 2 — CMSIS semaphore in callback:* Switched to `osSemaphoreRelease()`. Still silent after one print. The CMSIS wrapper is not reliably ISR-safe on all STM32 HAL versions.

*Attempt 3 — native FreeRTOS semaphore, missing UART interrupt:* Used `xSemaphoreGiveFromISR()` — correct API — but only one line printed then stopped. `HAL_UART_TxCpltCallback` was never firing because `USART2_IRQn` was not enabled. Both the DMA interrupt and the UART interrupt must be active for the callback chain to complete.

Final working solution uses `xSemaphoreGiveFromISR()` with both `DMA1_Stream6_IRQn` and `USART2_IRQn` enabled.

**Three new concepts this chapter introduced:**

`HAL_UART_TxCpltCallback` — a function HAL calls automatically when DMA finishes. You don't call it — the hardware triggers it via interrupt. TxCplt = Transmission Complete.

`xSemaphoreGiveFromISR` — the ISR-safe version of `xSemaphoreGive`. Normal FreeRTOS API calls assume they run inside a task. Calling them from an interrupt corrupts RTOS internals. Every FreeRTOS function that can be called from an ISR has a `FromISR` variant.

`portYIELD_FROM_ISR` — when the semaphore is given back inside an ISR, a waiting task becomes ready. Without this call that task waits until the scheduler next runs. With it, the task runs immediately when the interrupt exits. More responsive and correct.

---

## Concepts Summary

| Concept | What it solves | Key function |
|---|---|---|
| Pre-emptive multitasking | Multiple tasks running concurrently | `osDelay()` |
| Message Queue | Passing data between tasks safely | `osMessageQueuePut/Get()` |
| Mutex | Protecting shared hardware from race conditions | `osMutexAcquire/Release()` |
| Binary Semaphore | Signaling between tasks without data | `osSemaphoreRelease/Acquire()` |
| Task Notification | Lightweight direct signal between two tasks | `xTaskNotifyGive/ulTaskNotifyTake()` |
| Software Timer | Periodic callbacks without a dedicated task | `osTimerNew/Start()` |
| Priority Inversion | Bug where low priority blocks high priority | Fixed by mutex + priority inheritance |
| DMA + UART | Non-blocking transmission, CPU free during transfer | `HAL_UART_Transmit_DMA()` + `xSemaphoreGiveFromISR()` |

---

## What I Learned

The biggest shift was understanding that RTOS primitives are not interchangeable — each one exists for a specific reason:

- Use a **queue** when you need to pass data between tasks
- Use a **semaphore** when you just need to signal that something happened
- Use a **task notification** when signaling one specific task and you want zero overhead
- Use a **mutex** (not a semaphore) when protecting a shared resource — because only a mutex can prevent priority inversion
- Use a **software timer** for simple periodic actions that don't need a full task
- Use **DMA** when you want the CPU free during data transfers — but understand that async hardware requires ISR-safe synchronization primitives

The priority inversion chapter was the most conceptually satisfying. The DMA chapter was the most practically challenging — three failed attempts before it worked, each one teaching something specific about the boundary between task context and ISR context.

---

## Notes

- Built with AI assistance as a structured learning exercise
- Hardware: STM32 NUCLEO-F446RE
- Each branch is self-contained and can be flashed independently
