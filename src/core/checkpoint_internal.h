/* boxrig - Under MIT. */
#ifndef BOXRIG_CHECKPOINT_INTERNAL_H
#define BOXRIG_CHECKPOINT_INTERNAL_H

#include "terminal_internal.h"

/* The output grows only while exporting. Import borrows the input for the
 * duration of the call. All primitives stop at the first error. */
typedef struct boxrig_checkpoint_io {
    bool reading;
    boxrig_status status;
    const uint8_t *input;
    uint8_t *output;
    size_t offset;
    size_t capacity;
    size_t limit;
} boxrig_checkpoint_io;

void boxrig_cp_bytes(boxrig_checkpoint_io *io, void *data, size_t length);
void boxrig_cp_bool(boxrig_checkpoint_io *io, bool *value);
void boxrig_cp_u8(boxrig_checkpoint_io *io, uint8_t *value);
void boxrig_cp_u32(boxrig_checkpoint_io *io, uint32_t *value);
void boxrig_cp_i32(boxrig_checkpoint_io *io, int32_t *value);
void boxrig_cp_u64(boxrig_checkpoint_io *io, uint64_t *value);
void boxrig_cp_size(boxrig_checkpoint_io *io, size_t *value);
void boxrig_cp_fail(boxrig_checkpoint_io *io);
/* Read the representation through an integer object, independent of the
 * implementation's enum width. Each caller validates its own domain. */
#define boxrig_cp_enum(io, ptr) do { \
    uint32_t boxrig_enum_value = (io)->reading ? 0 : (uint32_t)*(ptr); \
    boxrig_cp_u32((io), &boxrig_enum_value); \
    if ((io)->reading && (io)->status == BOXRIG_STATUS_OK) *(ptr) = boxrig_enum_value; \
} while (0)

void boxrig_checkpoint_screen(boxrig_checkpoint_io *io, boxrig_terminal *terminal);
void boxrig_checkpoint_graphics(boxrig_checkpoint_io *io, boxrig_terminal *terminal);

#endif
