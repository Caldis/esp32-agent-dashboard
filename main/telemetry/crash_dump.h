/*
 * crash_dump — esp32 panic-handler hook + NVS-backed crash record.
 *
 * Flow on a panic:
 *   1. ESP-IDF panic handler invokes our `crash_dump_panic_hook()`
 *      via `esp_register_shutdown_handler()` (registered at boot).
 *   2. The hook serialises a fixed-shape struct (see crash_record_t)
 *      into the `crashdump` NVS namespace, blob key `last`. NVS
 *      writes are best-effort — a corrupted flash sector means the
 *      next boot just won't see a dump, which is fine.
 *   3. The chip resets normally.
 *
 * Flow on the next boot:
 *   4. crash_dump_init() checks the namespace; if a record exists,
 *      it caches its byte size and emits
 *         EVT: crash_dump_available bytes=N
 *      on the console.
 *   5. The bridge runs `dash dump crash`, which calls
 *      crash_dump_emit_and_clear() to print + delete the record.
 *
 * The on-device record is a single JSON object — see the §
 * "Crash record schema" in docs/TELEMETRY_SPEC.md and the example
 * in docs/OBSERVABILITY.md §5. Total bound: 1 KB after JSON encoding.
 *
 * Privacy: this record stays on-device. It is never auto-uploaded.
 * The user must explicitly copy + paste / gist / share it.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tail of the recent EVT stream we capture into the dump. Each entry
 * is up to 80 chars; 8 entries = 640 bytes worst case. */
#define CRASH_EVT_TAIL_LEN   8
#define CRASH_EVT_TEXT_MAX   80
#define CRASH_TASK_NAME_MAX  16
#define CRASH_REASON_MAX     32

typedef struct {
    /* Identity */
    char     schema[24];            /* "dash.crashdump/v1" */
    char     fw_version[16];        /* git tag at build time */
    uint32_t boot_count_at_crash;
    uint32_t uptime_s_at_crash;

    /* Reason */
    char     reason[CRASH_REASON_MAX];       /* "panic" / "watchdog" / "brownout" / "unknown" */
    int32_t  panic_reason_code;              /* esp_reset_reason_t enum */
    uint32_t panic_excvaddr;
    char     panic_text[CRASH_REASON_MAX];   /* "LoadProhibited", "InstrFetchProhibited", ... */
    char     task_name[CRASH_TASK_NAME_MAX]; /* TCB->pcTaskName at crash */
    uint32_t stack_pc;                       /* program counter */

    /* Memory at crash time */
    uint32_t heap_free_at_crash;

    /* Last few EVT lines for context. Index 0 = oldest. */
    char     evt_tail[CRASH_EVT_TAIL_LEN][CRASH_EVT_TEXT_MAX];
    uint16_t evt_tail_count;
} crash_record_t;

/* Initialise the in-RAM record + look for a stored one from the
 * previous boot. If found, emits `EVT: crash_dump_available
 * bytes=N` and increments the boot counter. Safe to call once at
 * boot, after nvs_flash_init().
 *
 * Returns true if a pending dump was detected. */
bool crash_dump_init(void);

/* Capture one EVT line into the in-RAM ring. The ring is the tail
 * we serialise into the dump on panic. NOT called automatically —
 * the dashboard wires this in by also calling it whenever it
 * invokes console_send_evt(). If never wired, dumps still work but
 * `evt_tail` is empty.
 *
 * Cheap (memcpy into ring). Safe to call from ISR-adjacent contexts
 * since we use a small atomic-flag spinlock. */
void crash_dump_record_evt(const char *line);

/* Manual record. Used by panic-handler hook OR by tests that want
 * to force a dump. `reason` is one of: panic, watchdog, brownout,
 * unknown. */
void crash_dump_capture(const char *reason,
                        int32_t panic_reason_code,
                        const char *panic_text,
                        const char *task_name,
                        uint32_t stack_pc,
                        uint32_t panic_excvaddr);

/* Format the cached record as JSON into `buf`. Returns bytes
 * written (excluding NUL), or -1 on overflow / no record. */
int crash_dump_to_json(char *buf, size_t buf_size);

/* Whether the boot detected a pending dump from the previous run. */
bool crash_dump_available(void);

/* Size of the JSON encoding of the pending dump (for the EVT line). */
int crash_dump_pending_bytes(void);

/* Emit the pending dump on the console as a tagged payload, then
 * clear it from NVS. Idempotent — safe to call when no dump
 * exists (sends "no crash dump available" reply). */
int crash_dump_emit_and_clear(void);

#ifdef __cplusplus
}
#endif
