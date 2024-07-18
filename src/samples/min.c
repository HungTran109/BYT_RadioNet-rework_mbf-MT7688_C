// Copyright (c) 2014-2017 JK Energy Ltd.
//
// Use authorized under the MIT license.

#include "min.h"

#define MIN_GET_ID(id_control) (id_control & (uint8_t)0xFFU)

// Number of bytes needed for a frame with a given payload length, excluding stuff bytes
// 3 header bytes, ID/control byte, length byte, seq byte, 4 byte CRC, EOF byte
#define NUMBER_OF_BYTE_NEED_FOR_A_FRAME_EX_STUFF (11U)
#define ON_WIRE_SIZE(p) ((p) + NUMBER_OF_BYTE_NEED_FOR_A_FRAME_EX_STUFF)

// Special protocol bytes
enum
{
    HEADER_BYTE = 0xAAU,
    STUFF_BYTE = 0x55U,
    EOF_BYTE = 0x55U,
};

// Receiving state machine
enum
{
    SEARCHING_FOR_SOF,
    RECEIVING_ID_CONTROL,
    RECEIVING_SEQ,
    RECEIVING_LENGTH,
    RECEIVING_PAYLOAD,
    RECEIVING_CHECKSUM_3,
    RECEIVING_CHECKSUM_2,
    RECEIVING_CHECKSUM_1,
    RECEIVING_CHECKSUM_0,
    RECEIVING_EOF,
};

static void crc32_init_context(min_crc32_context_t *context)
{
    context->crc = 0xFFFFFFFFU;
}


static void crc32_step(min_crc32_context_t *context, uint8_t byte)
{
    context->crc ^= byte;
#if MIN_CRC32_MINIMAL == 0
    for (uint32_t j = 0; j < 8; j++)
    {
        uint32_t mask = (uint32_t) - (context->crc & 1U);
        context->crc = (context->crc >> 1) ^ (0xEDB88320U & mask);
    }
#endif
}

static uint32_t crc32_finalize(min_crc32_context_t *context)
{
    return ~context->crc;
}

static void min_tx_byte(min_context_t *self, uint8_t byte)
{
    if (self->callback && self->callback->use_dma_frame == 0)
    {
        self->callback->tx_byte(self, byte);
    }
    else
    {
        self->tx_frame_payload_buf[self->tx_frame_bytes_count++] = byte;
        if (self->callback && self->tx_frame_bytes_count == self->tx_frame_payload_size)
        {
            if (self->callback->signal)
            {
                min_debug_print("Send tx signal\r\n");
                self->callback->signal(self, MIN_TX_FULL);
            }
            // if (self->callback->tx_frame)
            // {
            //     min_debug_print("Send tx frame\r\n");
            //     self->callback->tx_frame(self, self->tx_frame_payload_buf, MIN_MAX_PAYLOAD);
            // }
            self->tx_frame_bytes_count = 0;
        }
    }
}

static void stuffed_tx_byte(min_context_t *self, uint8_t byte)
{
    // Transmit the byte
    min_tx_byte(self, byte);
    crc32_step(&self->tx_checksum, byte);

    // See if an additional stuff byte is needed
    if (byte == HEADER_BYTE)
    {
        if (--self->tx_header_byte_countdown == 0)
        {
            min_tx_byte(self, STUFF_BYTE); // Stuff byte
            self->tx_header_byte_countdown = 2U;
        }
    }
    else
    {
        self->tx_header_byte_countdown = 2U;
    }
}

static void on_wire_bytes(min_context_t *self, 
                            uint8_t id_control, 
                            uint8_t seq, 
                            uint8_t *payload_base, 
                            uint16_t payload_offset, 
                            uint16_t payload_mask, 
                            uint8_t payload_len)
{
    uint8_t n, i;
    uint32_t checksum;

    self->tx_header_byte_countdown = 2U;
    crc32_init_context(&self->tx_checksum);

    if (self->callback && self->callback->signal)
        self->callback->signal(self, MIN_TX_BEGIN);

    // Header is 3 bytes; because unstuffed will reset receiver immediately
    min_tx_byte(self, HEADER_BYTE);
    min_tx_byte(self, HEADER_BYTE);
    min_tx_byte(self, HEADER_BYTE);

    stuffed_tx_byte(self, id_control);
    stuffed_tx_byte(self, payload_len);

    for (i = 0, n = payload_len; n > 0; n--, i++)
    {
        stuffed_tx_byte(self, payload_base[payload_offset]);
        payload_offset++;
        payload_offset &= payload_mask;
    }

    checksum = crc32_finalize(&self->tx_checksum);

    // Network order is big-endian. A decent C compiler will spot that this
    // is extracting bytes and will use efficient instructions.
    stuffed_tx_byte(self, (uint8_t)((checksum >> 24) & 0xFFU));
    stuffed_tx_byte(self, (uint8_t)((checksum >> 16) & 0xFFU));
    stuffed_tx_byte(self, (uint8_t)((checksum >> 8) & 0xFFU));
    stuffed_tx_byte(self, (uint8_t)((checksum >> 0) & 0xFFU));

    // Ensure end-of-frame doesn't contain 0xaa and confuse search for start-of-frame
    min_tx_byte(self, EOF_BYTE);

    if (self->callback && self->callback->signal)
        self->callback->signal(self, MIN_TX_END);

    if (self->callback && self->callback->use_dma_frame)
    {
        self->tx_frame_bytes_count = 0;
    }
}

// test only
static void stuffed_tx_output(uint8_t *output, 
                            uint32_t *size, 
                            uint32_t *crc, 
                            uint8_t *tx_header_byte_countdown, 
                            uint8_t byte)
{
    // Transmit the byte
    uint32_t tmp_size = *size;
    uint32_t tmp_crc = *crc;
    uint8_t tmp_header_cnt = *tx_header_byte_countdown;
    output[tmp_size++] = byte;

    tmp_crc ^= byte;
    for (uint32_t j = 0; j < 8; j++)
    {
        uint32_t mask = (uint32_t) - (tmp_crc & 1U);
        tmp_crc = (tmp_crc >> 1) ^ (0xEDB88320U & mask);
    }

    // See if an additional stuff byte is needed
    if (byte == HEADER_BYTE)
    {
        if (--tmp_header_cnt == 0)
        {
            output[tmp_size++] = STUFF_BYTE; // Stuff byte
            tmp_header_cnt = 2U;
        }
    }
    else
    {
        tmp_header_cnt = 2U;
    }

    *tx_header_byte_countdown = tmp_header_cnt;
    *crc = tmp_crc;
    *size = tmp_size;
}

static void stuffed_tx_size(uint32_t *size, 
                            uint32_t *crc, 
                            uint8_t *tx_header_byte_countdown, 
                            uint8_t byte)
{
    // Transmit the byte
    uint32_t tmp_size = *size;
    uint32_t tmp_crc = *crc;
    uint8_t tmp_header_cnt = *tx_header_byte_countdown;
    tmp_size++;

    tmp_crc ^= byte;
    for (uint32_t j = 0; j < 8; j++)
    {
        uint32_t mask = (uint32_t) - (tmp_crc & 1U);
        tmp_crc = (tmp_crc >> 1) ^ (0xEDB88320U & mask);
    }

    // See if an additional stuff byte is needed
    if (byte == HEADER_BYTE)
    {
        if (--tmp_header_cnt == 0)
        {
            tmp_size++; // Stuff byte
            tmp_header_cnt = 2U;
        }
    }
    else
    {
        tmp_header_cnt = 2U;
    }

    *tx_header_byte_countdown = tmp_header_cnt;
    *crc = tmp_crc;
    *size = tmp_size;
}

static uint32_t on_wire_output_size(uint8_t id_control,
                                    uint8_t seq,
                                    uint8_t *payload_base,
                                    uint16_t payload_offset,
                                    uint16_t payload_mask,
                                    uint8_t payload_len)
{
    uint8_t n, i;
    uint32_t checksum;
    uint8_t tx_header_byte_countdown = 2U;
    uint32_t init_crc = 0xFFFFFFFFU;
    uint32_t tmp_size = 0;

    // Header is 3 bytes; because unstuffed will reset receiver immediately
    tmp_size += 3;

    stuffed_tx_size(&tmp_size, &init_crc, &tx_header_byte_countdown, id_control);
    stuffed_tx_size(&tmp_size, &init_crc, &tx_header_byte_countdown, payload_len);

    for (i = 0, n = payload_len; n > 0; n--, i++)
    {
        stuffed_tx_size(&tmp_size, &init_crc, 
                        &tx_header_byte_countdown, 
                        payload_base[payload_offset]);
        payload_offset++;
        payload_offset &= payload_mask;
    }

    checksum = ~init_crc;

    // Network order is big-endian. A decent C compiler will spot that this
    // is extracting bytes and will use efficient instructions.
    stuffed_tx_size(&tmp_size, &init_crc, 
                    &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 24) & 0xFFU));
    stuffed_tx_size(&tmp_size, &init_crc, 
                    &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 16) & 0xFFU));
    stuffed_tx_size(&tmp_size, 
                    &init_crc, 
                    &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 8) & 0xFFU));
    stuffed_tx_size(&tmp_size, &init_crc, 
                    &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 0) & 0xFFU));

    // Ensure end-of-frame doesn't contain 0xaa and confuse search for start-of-frame
    tmp_size++; //EOF_BYTE;

    return tmp_size;
}

static void on_wire_output_buffer(uint8_t id_control,
                                  uint8_t seq,
                                  uint8_t *payload_base,
                                  uint16_t payload_offset,
                                  uint16_t payload_mask,
                                  uint8_t payload_len,
                                  uint8_t *output,
                                  uint32_t *size)
{
    uint8_t n, i;
    uint32_t checksum;
    uint8_t tx_header_byte_countdown = 2U;
    uint32_t init_crc = 0xFFFFFFFFU;
    *size = 0;
    uint32_t tmp_size = 0;

    // Header is 3 bytes; because unstuffed will reset receiver immediately
    output[tmp_size++] = HEADER_BYTE;
    output[tmp_size++] = HEADER_BYTE;
    output[tmp_size++] = HEADER_BYTE;

    stuffed_tx_output(output, &tmp_size, &init_crc, &tx_header_byte_countdown, id_control);
    stuffed_tx_output(output, &tmp_size, 
                        &init_crc, 
                        &tx_header_byte_countdown, 
                        payload_len);

    for (i = 0, n = payload_len; n > 0; n--, i++)
    {
        stuffed_tx_output(output, &tmp_size, 
                            &init_crc, &tx_header_byte_countdown, 
                            payload_base[payload_offset]);
        payload_offset++;
        payload_offset &= payload_mask;
    }

    checksum = ~init_crc;

    // Network order is big-endian. A decent C compiler will spot that this
    // is extracting bytes and will use efficient instructions.
    stuffed_tx_output(output, &tmp_size, 
                    &init_crc, &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 24) & 0xFFU));
    stuffed_tx_output(output, &tmp_size, &init_crc, 
                    &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 16) & 0xFFU));
    stuffed_tx_output(output, &tmp_size, 
                    &init_crc, &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 8) & 0xFFU));
    stuffed_tx_output(output, &tmp_size, 
                    &init_crc, &tx_header_byte_countdown, 
                    (uint8_t)((checksum >> 0) & 0xFFU));

    // Ensure end-of-frame doesn't contain 0xaa and confuse search for start-of-frame
    output[tmp_size++] = EOF_BYTE;
    *size = tmp_size;
}

// This runs the receiving half of the transport protocol, acknowledging frames received, discarding
// duplicates received, and handling RESET requests.
static void valid_frame_received(min_context_t *self)
{
    if (self->callback && self->callback->rx_callback)
    {
        uint8_t id_control = self->rx_frame_id_control;

        min_msg_t msg;
        msg.id = MIN_GET_ID(id_control);
        msg.payload = self->rx_frame_payload_buf;
        msg.len = self->rx_control;

        self->callback->rx_callback(self, &msg);
    }
}

static void rx_byte(min_context_t *self, uint8_t byte)
{
    // Regardless of state, three header bytes means "start of frame" and
    // should reset the frame buffer and be ready to receive frame data
    //
    // Two in a row in over the frame means to expect a stuff byte.
    uint32_t crc;

    if (self->rx_header_bytes_seen == 2)
    {
        self->rx_header_bytes_seen = 0;
        if (byte == HEADER_BYTE)
        {
            self->rx_frame_state = RECEIVING_ID_CONTROL;
            return;
        }
        if (byte == STUFF_BYTE)
        {
            /* Discard this byte; carry on receiving on the next character */
            return;
        }
        else
        {
            /* Something has gone wrong, give up on this frame and look for header again */
            self->rx_frame_state = SEARCHING_FOR_SOF;
            return;
        }
    }

    if (byte == HEADER_BYTE && self->rx_frame_state < RECEIVING_ID_CONTROL)
    {
        self->rx_header_bytes_seen++;
    }
    else
    {
        self->rx_header_bytes_seen = 0;
    }

    switch (self->rx_frame_state)
    {
    case SEARCHING_FOR_SOF:
        break;
    case RECEIVING_ID_CONTROL:
        self->rx_frame_id_control = byte;
        self->rx_frame_bytes_count = 0;
        crc32_init_context(&self->rx_checksum);
        crc32_step(&self->rx_checksum, byte);
        self->rx_frame_seq = 0;
        self->rx_frame_state = RECEIVING_LENGTH;
        break;
    case RECEIVING_SEQ:
        self->rx_frame_seq = byte;
        crc32_step(&self->rx_checksum, byte);
        self->rx_frame_state = RECEIVING_LENGTH;
        break;
    case RECEIVING_LENGTH:
        self->rx_frame_length = byte;
        self->rx_control = byte;
        crc32_step(&self->rx_checksum, byte);
        if (self->rx_frame_length > 0)
        {
            // Can reduce the RAM size by compiling limits to frame sizes
            if (self->rx_frame_length <= MIN_MAX_PAYLOAD)
            {
                self->rx_frame_state = RECEIVING_PAYLOAD;
            }
            else
            {
                // Frame dropped because it's longer than any frame we can buffer
                self->rx_frame_state = SEARCHING_FOR_SOF;
            }
        }
        else
        {
            self->rx_frame_state = RECEIVING_CHECKSUM_3;
        }
        break;
    case RECEIVING_PAYLOAD:
        self->rx_frame_payload_buf[self->rx_frame_bytes_count++] = byte;
        crc32_step(&self->rx_checksum, byte);
        if (--self->rx_frame_length == 0)
        {
            self->rx_frame_state = RECEIVING_CHECKSUM_3;
        }
        break;
    case RECEIVING_CHECKSUM_3:
        self->rx_frame_checksum = ((uint32_t)byte) << 24;
        self->rx_frame_state = RECEIVING_CHECKSUM_2;
        break;
    case RECEIVING_CHECKSUM_2:
        self->rx_frame_checksum |= ((uint32_t)byte) << 16;
        self->rx_frame_state = RECEIVING_CHECKSUM_1;
        break;
    case RECEIVING_CHECKSUM_1:
        self->rx_frame_checksum |= ((uint32_t)byte) << 8;
        self->rx_frame_state = RECEIVING_CHECKSUM_0;
        break;
    case RECEIVING_CHECKSUM_0:
        self->rx_frame_checksum |= byte;
        crc = crc32_finalize(&self->rx_checksum);
        if (self->rx_frame_checksum != crc)
        {
            // Frame fails the checksum and so is dropped
            self->rx_frame_state = SEARCHING_FOR_SOF;
        }
        else
        {
            // Checksum passes, go on to check for the end-of-frame marker
            self->rx_frame_state = RECEIVING_EOF;
        }
        break;
    case RECEIVING_EOF:
        if (byte == EOF_BYTE)
        {
            // Frame received OK, pass up data to handler
            valid_frame_received(self);
        }
        // else discard
        // Look for next frame */
        self->rx_frame_state = SEARCHING_FOR_SOF;
        break;
    default:
        // Should never get here but in case we do then reset to a safe state
        self->rx_frame_state = SEARCHING_FOR_SOF;
        break;
    }
}

// API call: sends received bytes into a MIN context and runs the transport timeouts
void min_rx_feed(min_context_t *self, uint8_t *buf, uint32_t buf_len)
{
    if (!self || !buf || buf_len == 0)
        return;

    if (self->callback 
        && self->callback->get_ms 
        && self->callback->use_timeout_method)
    {
        self->callback->last_rx_time = self->callback->get_ms();
    }

    for (uint32_t i = 0; i < buf_len; i++)
    {
        rx_byte(self, buf[i]);
    }
}

void min_init_context(min_context_t *self)
{
    if (!self)
        return;

    // Initialize context
    self->rx_header_bytes_seen = 0;
    self->rx_frame_state = SEARCHING_FOR_SOF;
}

uint32_t min_tx_space(min_context_t *self)
{
    if (self->callback && self->callback->tx_space)
        return self->callback->tx_space(self);
    return 255;
}

// Sends an application MIN frame on the wire (do not put into the transport queue)
void min_send_frame(min_context_t *self, min_msg_t *msg)
{
    if (!msg || msg->len > MIN_MAX_PAYLOAD)
    {
        return;
    }

    if ((ON_WIRE_SIZE(msg->len) <= min_tx_space(self)))
    {
        on_wire_bytes(self, MIN_GET_ID(msg->id), 0, (uint8_t*)msg->payload, 0, 0xFFFFU, msg->len);
    }
}

uint32_t min_estimate_frame_output_size(min_msg_t *input_msg)
{
    if (!input_msg || input_msg->len > MIN_MAX_PAYLOAD)
        return 0;
    return on_wire_output_size(MIN_GET_ID(input_msg->id), 0, input_msg->payload, 0, 0xFFFFU, input_msg->len);
}

void min_build_raw_frame_output(min_msg_t *input_msg, uint8_t *output, uint32_t *len)
{
    if (!input_msg || input_msg->len > MIN_MAX_PAYLOAD)
        return;

    *len = 0;

    on_wire_output_buffer(MIN_GET_ID(input_msg->id), 
                        0, input_msg->payload, 
                        0, 0xFFFFU, 
                        input_msg->len, 
                        output, 
                        len);
}

void min_timeout_poll(min_context_t *self)
{
    if (self->rx_frame_state != SEARCHING_FOR_SOF
        && self->callback 
        && self->callback->use_timeout_method 
        && self->callback->get_ms)
    {
        uint32_t now = self->callback->get_ms();
        uint32_t diff;
        if (now < self->callback->last_rx_time)
        {
            diff = (0xFFFFFFFF - self->callback->last_rx_time) + now;
        }
        else
        {
            diff = now - self->callback->last_rx_time;
        }

        if (diff >= self->callback->timeout_not_seen_rx)
        {
            self->callback->last_rx_time = now;
            if (self->callback->timeout_callback 
                && self->rx_frame_state != SEARCHING_FOR_SOF)
            {
                self->callback->timeout_callback(self);
            }
            crc32_init_context(&self->rx_checksum);
            self->rx_header_bytes_seen = 0;
            self->rx_frame_bytes_count = 0;
            self->rx_frame_seq = 0;
            self->rx_frame_length = 0;
            self->rx_frame_state = SEARCHING_FOR_SOF;
        }
    }
}


// bool min_decode_message(uint8_t *buffer, int length, min_msg_t *msg) 
// {
//     /*tatic */ uint8_t tmp_rx_frame_buf[MIN_MAX_PAYLOAD];      // Payload received so far
//     uint32_t tmp_rx_frame_checksum = 0;         // Checksum received over the wire
//     min_crc32_context_t tmp_rx_checksum;               // Calculated checksum for receiving frame
//     uint8_t tmp_rx_header_bytes_seen = 0;       // Countdown of header bytes to reset state
//     uint8_t tmp_rx_frame_state = SEARCHING_FOR_SOF;             // State of receiver
//     uint8_t tmp_rx_frame_bytes_count = 0;       // Length of payload received so far
//     uint8_t tmp_rx_frame_id_control = 0;        // ID and control bit of frame being received
//     uint8_t tmp_rx_frame_length = 0;            // Length of frame
//     uint8_t tmp_rx_control = 0;                 // Control byte
//     bool is_hdr_found = false;
//     for (int i = 0; i < length && msg == null; i++) 
//     {
//         uint8_t data = buffer[i];
//         if (tmp_rx_header_bytes_seen == 2) 
//         {
//             if (data == HEADER_BYTE) 
//             {
//                 tmp_rx_frame_state = RECEIVING_ID_CONTROL;
//                 tmp_rx_header_bytes_seen = 0;
//                 is_hdr_found = true;
//                 continue;
//             }
//             if (data == STUFF_BYTE) 
//             {
//                 /* Discard this byte; carry on receiving on the next character */
//                 continue;
//             } 
//             else 
//             {
//                 /* Something has gone wrong, give up on this frame and look for header again */
//                 tmp_rx_frame_state = SEARCHING_FOR_SOF;
//                 tmp_rx_header_bytes_seen = 0;
//                 continue;
//             }
//         }
//         if (data == HEADER_BYTE) 
//         {
//             if (!is_hdr_found) 
//             {
//                 tmp_rx_header_bytes_seen++;
//             }
//         }
//         switch (tmp_rx_frame_state) 
//         {
//             case SEARCHING_FOR_SOF:
//                 break;
//             case RECEIVING_ID_CONTROL:
//                 tmp_rx_frame_id_control = data;
//                 tmp_rx_frame_bytes_count = 0;
//                 crc32_init_context(&tmp_rx_checksum);
//                 crc32_step(&tmp_rx_checksum, data);
//                 tmp_rx_frame_state = RECEIVING_LENGTH;
//                 break;
//             case RECEIVING_SEQ:
//                 crc32_step(&tmp_rx_checksum, data);
//                 tmp_rx_frame_state = RECEIVING_LENGTH;
//                 break;
//             case RECEIVING_LENGTH:
//                 tmp_rx_frame_length = data;
//                 tmp_rx_control = data;
//                 crc32_step(&tmp_rx_checksum, data);
//                 if (tmp_rx_frame_length > 0) 
//                 {
//                     // Can reduce the RAM size by compiling limits to frame sizes
//                     if (tmp_rx_frame_length <= MIN_MAX_PAYLOAD) 
//                     {
//                         tmp_rx_frame_state = RECEIVING_PAYLOAD;
//                     } 
//                     else
//                     {
//                         // Frame dropped because it's longer than any frame we can buffer
//                         tmp_rx_frame_state = SEARCHING_FOR_SOF;
//                     }
//                 } 
//                 else 
//                 {
//                     tmp_rx_frame_state = RECEIVING_CHECKSUM_3;
//                 }
//                 break;
//             case RECEIVING_PAYLOAD:
//                 tmp_rx_frame_buf[tmp_rx_frame_bytes_count++] = data;
//                 crc32_step(&tmp_rx_checksum, data);
//                 if (--tmp_rx_frame_length == 0) 
//                 {
//                     tmp_rx_frame_state = RECEIVING_CHECKSUM_3;
//                 }
//                 break;
//             //CRC: 240 5 20 172
//             case RECEIVING_CHECKSUM_3:
//                 tmp_rx_frame_checksum = ((long) data << 24);
//                 tmp_rx_frame_state = RECEIVING_CHECKSUM_2;
//                 break;
//             case RECEIVING_CHECKSUM_2:
//                 tmp_rx_frame_checksum |= ((long) data << 16);
//                 tmp_rx_frame_state = RECEIVING_CHECKSUM_1;
//                 break;
//             case RECEIVING_CHECKSUM_1:
//                 tmp_rx_frame_checksum |= ((long) data << 8);
//                 tmp_rx_frame_state = RECEIVING_CHECKSUM_0;
//                 break;
//             case RECEIVING_CHECKSUM_0:
//                 tmp_rx_frame_checksum |= data;
//                 uint32_t crc = ~tmp_rx_frame_checksum;
//                 if (tmp_rx_checksum.crc != crc) 
//                 {
//                     //Timber.e("crc32 failed: %08X != %08X", crc, tmp_rx_frame_checksum);
//                     // Frame fails the checksum and so is dropped
//                     tmp_rx_frame_state = SEARCHING_FOR_SOF;
//                     minFrame = new MinFrame(getFrameId(tmp_rx_frame_id_control),
//                             tmp_rx_frame_buf, tmp_rx_control);
//                     is_hdr_found = false;
//                 } else {
//                     // Checksum passes, go on to check for the end-of-frame marker
//                     tmp_rx_frame_state = RECEIVING_EOF;
//                 }
//                 break;
//             case RECEIVING_EOF:
//                 if (data == EOF_BYTE) 
//                 {
//                     // Frame received OK, pass up data to handler
//                     //Timber.d("Valid frame received, id_control: " + tmp_rx_frame_id_control);
//                     minFrame = new MinFrame(getFrameId(tmp_rx_frame_id_control),
//                             tmp_rx_frame_buf, tmp_rx_control);
//                     is_hdr_found = false;
//                 }
//                 // Look for next frame */
//                 tmp_rx_frame_state = SEARCHING_FOR_SOF;
//                 break;
//             default:
//                 // Should never get here but in case we do then reset to a safe state
//                 tmp_rx_frame_state = SEARCHING_FOR_SOF;
//                 break;
//         }
//     }
//     return minFrame;
// }
