/**
 * @file  retarget.c
 * @brief Sends printf() to the service UART (USART3, PD8/PD9 at 115200 8N1).
 *
 * The application prints its boot banner, the self-test result and every fault
 * transition. Without this, newline's default `_write` returns -1 and all of
 * that is silently discarded — which on a board with four LEDs and no screen
 * removes the only way to find out why a self-test failed.
 *
 * Two things worth knowing:
 *
 * 1. Transmission is blocking. That is deliberate: a log line that is dropped
 *    because the UART was busy is worse than one that costs a few hundred
 *    microseconds, and nothing here prints from an interrupt.
 *
 * 2. It is safe before the scheduler starts. `evse_app_start()` prints during
 *    the power-on self-test, before any task exists, so this must not touch a
 *    FreeRTOS primitive.
 *
 * If you would rather use SWO/ITM through the ST-Link instead of a UART, define
 * EVSE_LOG_VIA_ITM=1 — but note ST-Link/V2 SWO needs the pin wired to PB3 and
 * is easy to get subtly wrong, whereas a USB-serial adapter on PD8 always works.
 */
#include "evse_board.h"
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef EVSE_LOG_VIA_ITM
#define EVSE_LOG_VIA_ITM 0
#endif

#if !EVSE_LOG_VIA_ITM
extern UART_HandleTypeDef DEBUG_UART_HANDLE;
#endif

/** Milliseconds to wait for the UART; long enough for a full line at 115200. */
#define LOG_TX_TIMEOUT_MS  100u

int _write(int file, char *ptr, int len)
{
    if (file != STDOUT_FILENO && file != STDERR_FILENO) {
        errno = EBADF;
        return -1;
    }
    if (ptr == NULL || len <= 0) {
        return 0;
    }

#if EVSE_LOG_VIA_ITM
    for (int i = 0; i < len; i++) {
        ITM_SendChar((uint32_t)(uint8_t)ptr[i]);
    }
#else
    /*
     * A failed transmit is reported as "wrote everything" rather than an error.
     * printf's return value is not checked anywhere in this firmware, and
     * returning -1 makes newlib mark the stream in error and stop calling us —
     * so one busy UART would silence logging for the rest of the run.
     */
    (void)HAL_UART_Transmit(&DEBUG_UART_HANDLE, (uint8_t *)ptr,
                            (uint16_t)len, LOG_TX_TIMEOUT_MS);
#endif
    return len;
}

/*
 * The remaining newlib stubs. Without them the linker pulls in the default
 * implementations, which drag in a lot of unused machinery and emit
 * "_close is not implemented and will always fail" warnings that bury real ones.
 */

int _close(int file)                          { (void)file; return -1; }
int _isatty(int file)                         { (void)file; return 1; }
int _lseek(int file, int ptr, int dir)        { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len)       { (void)file; (void)ptr; (void)len; return 0; }
int _fstat(int file, struct stat *st)         { (void)file; st->st_mode = S_IFCHR; return 0; }

/**
 * Called by newlib when it wants heap.
 *
 * This firmware allocates nothing at runtime — every task, stack and buffer is
 * static — so any call here means something unexpectedly wants malloc. Failing
 * rather than silently handing out memory makes that visible at the point it
 * happens instead of as heap exhaustion much later.
 *
 * printf itself is the usual culprit: newlib's full printf mallocs its internal
 * buffer. Link with `--specs=nano.specs` (the CMakeLists and the CubeIDE
 * defaults both do) to get the non-allocating variant.
 */
void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}
