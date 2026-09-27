/* bl_protocol.c
 *
 * Framed command protocol for the USB-CDC bootloader:
 *   frame = [TYPE:1][LEN:4 LE][PAYLOAD:LEN][CRC32:4 LE]
 * Reassembly is interrupt-safe: CDC_Receive_FS() feeds BL_OnUsbRx() and the
 * main loop calls BL_ProtocolPoll() which drains complete frames.
 */

#include "main.h"
#include "bl_protocol.h"
#include "usbd_cdc_if.h"
#include <string.h>

/* Implemented in main.c */
extern uint8_t JumpToApplication(void);

/* ------------------------------------------------------------------ */
/* CRC32 - zlib compatible (poly 0xEDB88320, reflected, init 0xFFFFFFFF) */
/* ------------------------------------------------------------------ */
static uint32_t s_crc_table[256];
static uint8_t  s_crc_table_ready = 0;
static uint32_t s_crc;                 /* running image CRC context */

static void crc32_table_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_table_ready = 1;
}

/* Pure one-shot CRC over a buffer - used for frames, never touches s_crc. */
static uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    if (!s_crc_table_ready) {
        crc32_table_init();
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c = (c >> 8) ^ s_crc_table[(c ^ data[i]) & 0xFFu];
    }
    return c ^ 0xFFFFFFFFu;
}

void BL_Crc32Reset(void)
{
    if (!s_crc_table_ready) {
        crc32_table_init();
    }
    s_crc = 0xFFFFFFFFu;
}

void BL_Crc32Update(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        s_crc = (s_crc >> 8) ^ s_crc_table[(s_crc ^ data[i]) & 0xFFu];
    }
}

uint32_t BL_Crc32Value(void)
{
    return s_crc ^ 0xFFFFFFFFu;
}

uint32_t BL_Crc32Compute(const uint8_t *data, uint32_t len)
{
    return crc32_buf(data, len);
}

/* ------------------------------------------------------------------ */
/* RX ring buffer (ISR producer, main-loop consumer)                    */
/* ------------------------------------------------------------------ */
#define RX_RING_SIZE  2048u            /* power of two, > max frame (1033) */
#define RX_RING_MASK  (RX_RING_SIZE - 1u)

static uint8_t          s_rx_ring[RX_RING_SIZE];
static volatile uint32_t s_rx_head = 0;
static volatile uint32_t s_rx_tail = 0;

static uint32_t ring_count(void)
{
    return (s_rx_head - s_rx_tail) & RX_RING_MASK;
}

static uint8_t ring_peek(uint32_t offset)
{
    return s_rx_ring[(s_rx_tail + offset) & RX_RING_MASK];
}

static void ring_drop(uint32_t count)
{
    s_rx_tail = (s_rx_tail + count) & RX_RING_MASK;
}

/* Called from CDC_Receive_FS (ISR context). Drops bytes if the ring is full. */
void BL_OnUsbRx(const uint8_t *buf, uint32_t len)
{
    /* DIAGNOSTIC: toggle Blue LED (LD4) whenever any USB OUT data arrives. */
    if (len > 0u) {
        HAL_GPIO_TogglePin(LD4_GPIO_Port, LD4_Pin);
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (s_rx_head + 1u) & RX_RING_MASK;
        if (next == s_rx_tail) {
            break; /* overflow: drop remaining bytes */
        }
        s_rx_ring[s_rx_head] = buf[i];
        s_rx_head = next;
    }
}

/* ------------------------------------------------------------------ */
/* Protocol state                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    BL_STATE_IDLE = 0,
    BL_STATE_ERASED,
    BL_STATE_WRITTEN,
    BL_STATE_VERIFIED
} BL_State_t;

#define APP_METADATA_MAGIC   0x424C4452u   /* "BLDR" */

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
    uint32_t reserved;
} BL_Metadata_t;

static BL_State_t s_state         = BL_STATE_IDLE;
static uint32_t   s_write_addr    = 0;
static uint32_t   s_expected_size = 0;
static uint32_t   s_bytes_written = 0;

static uint8_t s_frame_buf[5u + BL_MAX_PAYLOAD_SIZE + 4u];

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Responses                                                            */
/* ------------------------------------------------------------------ */
static void BL_SendResponse(uint8_t status, const uint8_t *payload, uint32_t len)
{
    uint8_t buf[1u + 4u + 8u + 4u];

    if (len > 8u) {
        len = 8u;
    }
    buf[0] = status;
    write_le32(&buf[1], len);
    if (payload != NULL && len > 0u) {
        memcpy(&buf[5], payload, len);
    }
    write_le32(&buf[5 + len], crc32_buf(buf, 5u + len));

    /* Retry briefly while a previous transfer is still in flight. */
    for (uint32_t attempt = 0; attempt < 50u; attempt++) {
        if (CDC_Transmit_FS(buf, (uint16_t)(5u + len + 4u)) != USBD_BUSY) {
            break;
        }
        HAL_Delay(1);
    }
}

/* ------------------------------------------------------------------ */
/* Validation helpers                                                   */
/* ------------------------------------------------------------------ */
uint8_t BL_VectorTableValid(void)
{
    uint32_t sp    = *(volatile uint32_t *)APP_FLASH_BASE_ADDRESS;
    uint32_t reset = *(volatile uint32_t *)(APP_FLASH_BASE_ADDRESS + 4u);

    /* Initial stack pointer must land inside the 40K SRAM. */
    if (sp < SRAM_BASE || sp > (SRAM_BASE + 40u * 1024u)) {
        return 0;
    }
    /* Reset handler must point into the application region (Thumb). */
    if (reset < APP_FLASH_BASE_ADDRESS || reset >= APP_FLASH_END) {
        return 0;
    }
    if ((reset & 1u) == 0u) {
        return 0;
    }
    return 1;
}

uint8_t BL_ImageIsValid(void)
{
    const BL_Metadata_t *meta = (const BL_Metadata_t *)APP_METADATA_ADDRESS;

    if (meta->magic != APP_METADATA_MAGIC) {
        return 0;
    }
    if (meta->size == 0u ||
        meta->size > (APP_FLASH_WRITABLE_END - APP_FLASH_BASE_ADDRESS)) {
        return 0;
    }
    return crc32_buf((const uint8_t *)APP_FLASH_BASE_ADDRESS, meta->size)
           == meta->crc32;
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                     */
/* ------------------------------------------------------------------ */
static void handle_ping(const uint8_t *payload, uint32_t len)
{
    uint8_t ver[4];
    (void)payload;
    (void)len;
    write_le32(ver, BOOTLOADER_VERSION); /* only low 2 bytes used */
    BL_SendResponse(BL_STATUS_OK, ver, 2u);
}

static void handle_erase(const uint8_t *payload, uint32_t len)
{
    if (len != 4u) {
        BL_SendResponse(BL_STATUS_ERR_PARAM, NULL, 0);
        return;
    }
    uint32_t size = read_le32(payload);
    if (size == 0u ||
        size > (APP_FLASH_WRITABLE_END - APP_FLASH_BASE_ADDRESS)) {
        BL_SendResponse(BL_STATUS_ERR_BOUNDS, NULL, 0);
        return;
    }

    FLASH_EraseInitTypeDef erase;
    uint32_t page_error = 0;
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_FLASH_BASE_ADDRESS;
    erase.NbPages     = (size + 2047u) / 2048u;   /* only what is needed */

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef rc = HAL_FLASHEx_Erase(&erase, &page_error);
    HAL_FLASH_Lock();
    if (rc != HAL_OK) {
        BL_SendResponse(BL_STATUS_ERR_FLASH, NULL, 0);
        return;
    }

    s_write_addr    = APP_FLASH_BASE_ADDRESS;
    s_expected_size = size;
    s_bytes_written = 0;
    BL_Crc32Reset();
    s_state = BL_STATE_ERASED;
    BL_SendResponse(BL_STATUS_OK, NULL, 0);
}

static void handle_write(uint8_t *payload, uint32_t len)
{
    if (s_state != BL_STATE_ERASED && s_state != BL_STATE_WRITTEN) {
        BL_SendResponse(BL_STATUS_ERR_STATE, NULL, 0);
        return;
    }
    if (len == 0u) {
        BL_SendResponse(BL_STATUS_ERR_PARAM, NULL, 0);
        return;
    }
    if (s_bytes_written + len > s_expected_size) {
        BL_SendResponse(BL_STATUS_ERR_BOUNDS, NULL, 0);
        return;
    }

    uint32_t padded = (len + 3u) & ~3u;
    if (s_write_addr + padded > APP_FLASH_WRITABLE_END) {
        BL_SendResponse(BL_STATUS_ERR_BOUNDS, NULL, 0);
        return;
    }

    /* Pad the final word with 0xFF (erased flash value). */
    for (uint32_t i = len; i < padded; i++) {
        payload[i] = 0xFFu;
    }

    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < padded; i += 4u) {
        uint32_t word = read_le32(&payload[i]);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              s_write_addr, word) != HAL_OK) {
            HAL_FLASH_Lock();
            BL_SendResponse(BL_STATUS_ERR_FLASH, NULL, 0);
            return;
        }
        s_write_addr += 4u;
    }
    HAL_FLASH_Lock();

    /* CRC covers real payload bytes only, matching the host file CRC. */
    BL_Crc32Update(payload, len);
    s_bytes_written += len;
    s_state = BL_STATE_WRITTEN;
    BL_SendResponse(BL_STATUS_OK, NULL, 0);
}

static void handle_verify(const uint8_t *payload, uint32_t len)
{
    if (len != 4u) {
        BL_SendResponse(BL_STATUS_ERR_PARAM, NULL, 0);
        return;
    }
    if (s_state != BL_STATE_WRITTEN) {
        BL_SendResponse(BL_STATUS_ERR_STATE, NULL, 0);
        return;
    }
    if (s_bytes_written != s_expected_size) {
        BL_SendResponse(BL_STATUS_ERR_VERIFY, NULL, 0);
        return;
    }
    if (BL_Crc32Value() != read_le32(payload)) {
        BL_SendResponse(BL_STATUS_ERR_VERIFY, NULL, 0);
        return;
    }

    /* Persist a metadata record on the reserved last page so the
     * bootloader can re-validate the image on every power-up. */
    BL_Metadata_t meta;
    meta.magic    = APP_METADATA_MAGIC;
    meta.size     = s_bytes_written;
    meta.crc32    = BL_Crc32Value();
    meta.reserved = 0xFFFFFFFFu;

    FLASH_EraseInitTypeDef erase;
    uint32_t page_error = 0;
    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_METADATA_ADDRESS;
    erase.NbPages     = 1u;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef rc = HAL_FLASHEx_Erase(&erase, &page_error);
    if (rc == HAL_OK) {
        const uint32_t *words = (const uint32_t *)&meta;
        for (uint32_t i = 0; i < sizeof(BL_Metadata_t) / 4u; i++) {
            rc = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                   APP_METADATA_ADDRESS + i * 4u,
                                   words[i]);
            if (rc != HAL_OK) {
                break;
            }
        }
    }
    HAL_FLASH_Lock();

    if (rc != HAL_OK) {
        BL_SendResponse(BL_STATUS_ERR_FLASH, NULL, 0);
        return;
    }

    s_state = BL_STATE_VERIFIED;
    BL_SendResponse(BL_STATUS_OK, NULL, 0);
}

static void handle_jump(const uint8_t *payload, uint32_t len)
{
    (void)payload;
    (void)len;

    if (s_state != BL_STATE_VERIFIED) {
        BL_SendResponse(BL_STATUS_ERR_STATE, NULL, 0);
        return;
    }
    if (!BL_VectorTableValid()) {
        BL_SendResponse(BL_STATUS_ERR_FLASH, NULL, 0);
        return;
    }

    BL_SendResponse(BL_STATUS_OK, NULL, 0);
    HAL_Delay(50);               /* let the OK frame reach the host */
    JumpToApplication();         /* re-validates, then does not return */
}

/* ------------------------------------------------------------------ */
/* Frame dispatch                                                       */
/* ------------------------------------------------------------------ */
void BL_ProtocolInit(void)
{
    s_rx_head        = 0;
    s_rx_tail        = 0;
    s_state          = BL_STATE_IDLE;
    s_write_addr     = 0;
    s_expected_size  = 0;
    s_bytes_written  = 0;
    if (!s_crc_table_ready) {
        crc32_table_init();
    }
}

void BL_ProtocolPoll(void)
{
    uint32_t avail = ring_count();
    if (avail < 5u) {
        return;                       /* need at least TYPE + LEN */
    }

    uint32_t len = (uint32_t)ring_peek(1) |
                   ((uint32_t)ring_peek(2) << 8) |
                   ((uint32_t)ring_peek(3) << 16) |
                   ((uint32_t)ring_peek(4) << 24);

    if (len > BL_MAX_PAYLOAD_SIZE) {
        ring_drop(1u);                /* desynced stream: resync one byte */
        return;
    }

    uint32_t frame_len = 5u + len + 4u;
    if (avail < frame_len) {
        return;                       /* incomplete frame, wait for more */
    }

    for (uint32_t i = 0; i < frame_len; i++) {
        s_frame_buf[i] = ring_peek(i);
    }
    ring_drop(frame_len);

    if (crc32_buf(s_frame_buf, 5u + len) !=
        read_le32(&s_frame_buf[5u + len])) {
        BL_SendResponse(BL_STATUS_ERR_CRC, NULL, 0);
        return;
    }

    /* DIAGNOSTIC: toggle Green LED (LD6) whenever a full, CRC-valid frame
     * is successfully parsed and about to be dispatched. */
    HAL_GPIO_TogglePin(LD6_GPIO_Port, LD6_Pin);

    uint8_t  cmd     = s_frame_buf[0];
    uint8_t *payload = &s_frame_buf[5];

    switch (cmd) {
    case BL_CMD_PING:   handle_ping(payload, len);   break;
    case BL_CMD_ERASE:  handle_erase(payload, len);  break;
    case BL_CMD_WRITE:  handle_write(payload, len);  break;
    case BL_CMD_VERIFY: handle_verify(payload, len); break;
    case BL_CMD_JUMP:   handle_jump(payload, len);   break;
    default:
        BL_SendResponse(BL_STATUS_ERR_UNKNOWN, NULL, 0);
        break;
    }
}
