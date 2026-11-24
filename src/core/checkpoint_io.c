/* boxrig - Under MIT. */
#include "checkpoint_internal.h"

#include <stdlib.h>
#include <string.h>

void boxrig_cp_fail(boxrig_checkpoint_io *io)
{
    if (io->status == BOXRIG_STATUS_OK) io->status = BOXRIG_STATUS_INVALID_ARGUMENT;
}

void boxrig_cp_bytes(boxrig_checkpoint_io *io, void *data, size_t length)
{
    if (io->status != BOXRIG_STATUS_OK || length == 0) return;
    if (length > io->limit - io->offset) {
        io->status = io->reading ? BOXRIG_STATUS_INVALID_ARGUMENT : BOXRIG_STATUS_LIMIT_EXCEEDED;
        return;
    }
    if (io->reading) {
        memcpy(data, io->input + io->offset, length);
    } else {
        size_t required = io->offset + length;
        if (required > io->capacity) {
            size_t capacity = io->capacity ? io->capacity : 4096;
            if (capacity > io->limit) capacity = io->limit;
            while (capacity < required)
                capacity = capacity > io->limit / 2 ? io->limit : capacity * 2;
            uint8_t *bytes = realloc(io->output, capacity);
            if (!bytes) {
                io->status = BOXRIG_STATUS_OUT_OF_MEMORY;
                return;
            }
            io->output = bytes;
            io->capacity = capacity;
        }
        memcpy(io->output + io->offset, data, length);
    }
    io->offset += length;
}

void boxrig_cp_u8(boxrig_checkpoint_io *io, uint8_t *value)
{
    boxrig_cp_bytes(io, value, 1);
}

void boxrig_cp_bool(boxrig_checkpoint_io *io, bool *value)
{
    uint8_t byte = io->reading ? 0 : (uint8_t)*value;
    boxrig_cp_u8(io, &byte);
    if (byte > 1) boxrig_cp_fail(io);
    if (io->reading) *value = byte == 1;
}

static uint64_t little_endian(boxrig_checkpoint_io *io, uint64_t value, size_t length)
{
    uint8_t bytes[8];
    if (!io->reading) {
        for (size_t i = 0; i < length; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
    }
    boxrig_cp_bytes(io, bytes, length);
    if (io->reading && io->status == BOXRIG_STATUS_OK) {
        for (size_t i = 0; i < length; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    }
    return value;
}

void boxrig_cp_u32(boxrig_checkpoint_io *io, uint32_t *value)
{
    uint64_t result = little_endian(io, io->reading ? 0 : *value, 4);
    if (io->reading) *value = (uint32_t)result;
}

void boxrig_cp_i32(boxrig_checkpoint_io *io, int32_t *value)
{
    uint32_t bits = io->reading ? 0 : (uint32_t)*value;
    boxrig_cp_u32(io, &bits);
    if (io->reading) *value = bits <= INT32_MAX ? (int32_t)bits : -1 - (int32_t)(UINT32_MAX - bits);
}

void boxrig_cp_u64(boxrig_checkpoint_io *io, uint64_t *value)
{
    uint64_t result = little_endian(io, io->reading ? 0 : *value, 8);
    if (io->reading) *value = result;
}

void boxrig_cp_size(boxrig_checkpoint_io *io, size_t *value)
{
    uint64_t bits = io->reading ? 0 : *value;
    boxrig_cp_u64(io, &bits);
    if (bits > SIZE_MAX) boxrig_cp_fail(io);
    if (io->reading) *value = (size_t)bits;
}
