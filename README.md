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
feature/dma-uart            ← coming soon
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

**Why a semaphore causes the bug but a mutex fixes it:**

Semaphores have no concept of ownership. No task "owns" a semaphore — anyone can release it. Because of this, a semaphore cannot implement priority inheritance. It has no way to know which task is holding it.

A mutex tracks ownership. FreeRTOS knows exactly which task holds it. That ownership is what enables priority inheritance — the scheduler can promote the right task because it knows who to promote.

**Historical note:** This exact bug caused the Mars Pathfinder rover to reset repeatedly in 1997. A high priority task kept getting blocked by a low priority task holding a mutex, causing a watchdog timer to reset the entire system. The JPL engineers diagnosed and fixed it remotely — from Earth, on Mars.

---

### Chapter 6 — DMA + UART *(coming soon)*

Currently `UART_Print()` uses `HAL_UART_Transmit()` — a blocking call. The CPU sends every byte manually and waits until transmission completes before returning.

DMA (Direct Memory Access) offloads this work to dedicated hardware. The CPU hands the data to the DMA controller and returns immediately. DMA sends the bytes in the background and fires an interrupt when done.

The interesting challenge: the UART mutex must be released in the DMA completion callback, not after the function call — because the function returns before transmission is actually finished.

This branch is in progress.

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
| DMA + UART | Non-blocking transmission, CPU free during transfer | `HAL_UART_Transmit_DMA()` |

---

## What I Learned

The biggest shift was understanding that RTOS primitives are not interchangeable — each one exists for a specific reason:

- Use a **queue** when you need to pass data between tasks
- Use a **semaphore** when you just need to signal that something happened
- Use a **task notification** when signaling one specific task and you want zero overhead
- Use a **mutex** (not a semaphore) when protecting a shared resource — because only a mutex can prevent priority inversion
- Use a **software timer** for simple periodic actions that don't need a full task

The priority inversion chapter was the most valuable. It turned an abstract lecture concept into something visible and tangible — a bug I could see in the serial output, understand exactly why it was happening, and fix with a single change.

---

## Notes

- Built with AI assistance as a structured learning exercise
- Hardware: STM32 NUCLEO-F446RE
- Each branch is self-contained and can be flashed independently
