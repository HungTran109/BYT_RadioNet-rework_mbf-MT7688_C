// MIN Protocol v2.0.
//
// MIN is a lightweight reliable protocol for exchanging information from a microcontroller (MCU) to a host.
// It is designed to run on an 8-bit MCU but also scale up to more powerful devices. A typical use case is to
// send data from a UART on a small MCU over a UART-USB converter plugged into a PC host. A Python implementation
// of host code is provided (or this code could be compiled for a PC).
//
// MIN supports frames of 0-255 bytes (with a lower limit selectable at compile time to reduce RAM). MIN frames
// have identifier values between 0 and 63.
//
// An optional transport layer T-MIN can be compiled in. This provides sliding window reliable transmission of frames.
//
// Compile options:
//
// -  Define NO_TRANSPORT_PROTOCOL to remove the code and other overheads of dealing with transport frames. Any
//    transport frames sent from the other side are dropped.
//
// -  Define MIN_MAX_PAYLOAD if the size of the frames is to be limited. This is particularly useful with the transport
//    protocol where a deep FIFO is wanted but not for large frames.
//
// The API is as follows:
//
// -  min_init_context()
//    A MIN context is a structure allocated by the programmer that stores details of the protocol. This permits
//    the code to be reentrant and multiple serial ports to be used. The port parameter is used in a callback to
//    allow the programmer's serial port drivers to place bytes in the right port. In a typical scenario there will
//    be just one context.
//
// -  min_send_frame()
//    This sends a non-transport frame and will be dropped if the line is noisy.
//
// -  min_queue_frame()
//    This queues a transport frame which will will be retransmitted until the other side receives it correctly.
//
// -  min_rx_feed()
//    This passes in received bytes to the context associated with the source. Note that if the transport protocol
//    is included then this must be called regularly to operate the transport state machine even if there are no
//    incoming bytes.
//
// There are several callbacks: these must be provided by the programmer and are called by the library:
//
// -  min_tx_space()
//    The programmer's serial drivers must return the number of bytes of space available in the sending buffer.
//    This helps cut down on the number of lost frames (and hence improve throughput) if a doomed attempt to transmit a
//    frame can be avoided.
//
// -  min_tx_byte()
//    The programmer's drivers must send a byte on the given port. The implementation of the serial port drivers
//    is in the domain of the programmer: they might be interrupt-based, polled, etc.
//
// -  min_application_handler()
//    This is the callback that provides a MIN frame received on a given port to the application. The programmer
//    should then deal with the frame as part of the application.
//
// -  min_time_ms()
//    This is called to obtain current time in milliseconds. This is used by the MIN transport protocol to drive
//    timeouts and retransmits.

#ifndef MIN_H
#define MIN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef ASSERTION_CHECKING
#include <assert.h>
#endif

#ifndef MIN_CRC32_MINIMAL
#define MIN_CRC32_MINIMAL       0
#endif

#ifndef MIN_MAX_PAYLOAD
#define MIN_MAX_PAYLOAD (254U)
#endif

#ifndef MIN_DEBUG_PRINTING
#define MIN_DEBUG_PRINTING 0
#endif

#if MIN_DEBUG_PRINTING
// Debug print
void min_debug_print(const char *msg, ...);
#else
#define min_debug_print(...)
#endif

#if (MIN_MAX_PAYLOAD > 254)
#error "MIN frame payloads can be no bigger than 255 bytes"
#endif

#define MIN_DEFAULT_CONFIG()    {   \
                                    .get_ms = NULL,   \
                                    .last_rx_time = 0x00,   \
                                    .rx_callback = (void*)0,   \
                                    .signal = (void*)0,   \
                                    .timeout_callback = (void*)0,   \
                                    .timeout_not_seen_rx = 100,   \
                                    .tx_byte = (void*)0,   \
                                    .tx_frame = (void*)0,   \
                                    .tx_space = (void*)0,   \
                                    .use_dma_frame = 0,   \
                                    .use_timeout_method = 0,   \
                                }

typedef struct
{
    uint32_t crc;
} min_crc32_context_t;


typedef struct
{
    uint8_t id;
    uint8_t len;
    void *payload;
} min_msg_t;

typedef enum
{
    MIN_TX_BEGIN,
    MIN_TX_FULL,
    MIN_TX_END
} min_tx_signal_t;

typedef bool (*min_tx_byte_cb_t)(void *ctx, uint8_t data);
typedef void (*min_rx_frame_cb_t)(void *ctx, min_msg_t *frame);
typedef void (*min_rx_timeout_cb_t)(void *ctx);
typedef void (*min_tx_dma_cb_t)(void *ctx, uint8_t *msg, uint8_t len);
typedef uint32_t (*min_tx_fifo_space_available_cb_t)(void *ctx);
typedef void (*min_frame_signal_cb_t)(void *ctx, min_tx_signal_t signal);
typedef uint32_t (*min_get_ms_cb_t)(void);

typedef struct
{
    min_rx_frame_cb_t rx_callback;
    min_rx_frame_cb_t invalid_crc_callback;
    min_rx_timeout_cb_t timeout_callback;
    min_tx_byte_cb_t tx_byte;
    min_tx_dma_cb_t tx_frame;           // For DMA
    min_tx_fifo_space_available_cb_t tx_space;
    min_frame_signal_cb_t signal;
    min_get_ms_cb_t get_ms;
    bool use_dma_frame;
    bool use_timeout_method;
    uint32_t timeout_not_seen_rx;
    uint32_t last_rx_time;
} min_frame_cfg_t;

typedef struct
{
    min_frame_cfg_t *callback;
    uint8_t *rx_frame_payload_buf;      // Payload received so far
    uint8_t *tx_frame_payload_buf;      // Payload tx
    uint32_t tx_frame_payload_size;     // Max size of tx payload buffer
    uint32_t rx_frame_checksum;         // Checksum received over the wire
    min_crc32_context_t rx_checksum;    // Calculated checksum for receiving frame
    min_crc32_context_t tx_checksum;    // Calculated checksum for sending frame
    uint8_t rx_header_bytes_seen;       // Countdown of header bytes to reset state
    uint8_t rx_frame_state;             // State of receiver
    uint8_t rx_frame_bytes_count;       // Length of payload received so far
    uint8_t tx_frame_bytes_count;       // Length of payload received so far
    uint8_t rx_frame_id_control;        // ID and control bit of frame being received
    uint8_t rx_frame_seq;               // Sequence number of frame being received
    uint8_t rx_frame_length;            // Length of frame
    uint8_t rx_control;                 // Control byte
    uint8_t tx_header_byte_countdown;   // Count out the header bytes
} min_context_t;

/**
 * @brief       Send min frame
 * @param[in]   self Min context
 * @param[in]   msg Pointer to min message
 */
void min_send_frame(min_context_t *self, min_msg_t *msg);

/**
 * @brief       This function is called when new uart data received
 * @param[in]   self Min context
 * @param[in]   buf Buffer data
 * @param[in]   buf_len Length of data
 */
void min_rx_feed(min_context_t *self, uint8_t *buf, uint32_t buf_len);

/**
 * @brief       Init min protocol
 * @param[in]   self Min context
 */
void min_init_context(min_context_t *self);

/**
 * @brief       Estimate frame output size, suitable for DMA
 * @retval      Frame output size
 */
uint32_t min_estimate_frame_output_size(min_msg_t *input_msg);

// Test only
/**
 * @brief       Build raw frame output
 * @param[in]   input_msg Inout message
 * @param[out]  output Output buffer hold data
 * @param[out]  len Output buffer len
 * @note        Useful for DMA TX
 */
void min_build_raw_frame_output(min_msg_t *input_msg, uint8_t *output, uint32_t *len);

/**
 * @brief       Call this function to reset frame when timeout no received data
 */
void min_timeout_poll(min_context_t *self);


#endif /* MIN_H */
