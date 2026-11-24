/* boxrig - Under MIT. */
#include "boxrig/terminal.h"
#include "../src/core/terminal_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    boxrig_terminal *source = NULL;
    boxrig_terminal *destination = NULL;
    uint8_t *data = NULL;
    size_t length = 0;
    size_t consumed = 0;
    boxrig_image_handle original_handle = 0;
    boxrig_image_handle restored_handle = 0;
    boxrig_image_view original_image;
    boxrig_image_view restored_image;
    boxrig_image_placement_view original_placement;
    boxrig_image_placement_view restored_placement;
    const char *image = "\033_Ga=T,i=9,p=1,f=32,s=1,v=1,c=1,r=1,C=1;AQIDBA==\033\\";
    const char *frame = "\033_Ga=f,i=9,f=32,s=1,v=1,X=1,z=20,q=2;BAIDBA==\033\\";
    const char *child = "\033_Ga=T,i=10,p=1,P=9,Q=1,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AQIDBA==\033\\";
    config.rows = 2;
    config.columns = 4;
    assert(boxrig_terminal_create(&config, &source) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_set_cell_pixel_geometry(source, 1, 1) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_feed(source, (const uint8_t *)image,
        strlen(image), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(image));
    assert(boxrig_terminal_feed(source, (const uint8_t *)frame,
        strlen(frame), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(frame));
    assert(boxrig_terminal_feed(source, (const uint8_t *)child,
        strlen(child), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(child));
    assert(boxrig_terminal_image_count(source) == 2);
    assert(boxrig_terminal_image_placement_count(source) == 2);
    assert(source->graphics_protocol.animation_frame_count == 1);
    assert(boxrig_terminal_export_checkpoint(source, 1024 * 1024,
        &data, &length) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_import_checkpoint(data, length, 1024 * 1024,
        &destination) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_image_count(destination) == 2);
    assert(boxrig_terminal_image_placement_count(destination) == 2);
    assert(destination->graphics_protocol.animation_frame_count == 1);
    assert(boxrig_terminal_image_handle_at(source, 0, &original_handle) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_image_handle_at(destination, 0, &restored_handle) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image(source, original_handle, &original_image) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image(destination, restored_handle, &restored_image) == BOXRIG_STATUS_OK);
    assert(original_image.image_id == restored_image.image_id);
    assert(original_image.rgba_length == restored_image.rgba_length);
    assert(memcmp(original_image.rgba, restored_image.rgba,
        original_image.rgba_length) == 0);
    assert(boxrig_terminal_get_image_placement(source, 0,
        &original_placement) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image_placement(destination, 0,
        &restored_placement) == BOXRIG_STATUS_OK);
    assert(original_placement.image_id == restored_placement.image_id);
    assert(original_placement.placement_id == restored_placement.placement_id);
    assert(original_placement.columns == restored_placement.columns);
    assert(original_placement.rows == restored_placement.rows);
    assert(boxrig_terminal_get_image_placement(source, 1,
        &original_placement) == BOXRIG_STATUS_OK);
    assert(boxrig_terminal_get_image_placement(destination, 1,
        &restored_placement) == BOXRIG_STATUS_OK);
    assert(original_placement.kind == BOXRIG_IMAGE_PLACEMENT_RELATIVE);
    assert(restored_placement.kind == original_placement.kind);
    assert(restored_placement.image_id == original_placement.image_id);
    const char *unfinished = "\033_Ga=t,i=11,f=32,s=1,v=1,m=1;AQID";
    assert(boxrig_terminal_feed(source, (const uint8_t *)unfinished,
        strlen(unfinished), &consumed) == BOXRIG_STATUS_OK);
    assert(consumed == strlen(unfinished));
    uint8_t *rejected = NULL;
    size_t rejected_length = 0;
    assert(boxrig_terminal_export_checkpoint(source, 1024 * 1024,
        &rejected, &rejected_length) == BOXRIG_STATUS_BUSY);
    assert(rejected == NULL && rejected_length == 0);
    boxrig_checkpoint_free(data);
    boxrig_terminal_destroy(destination);
    boxrig_terminal_destroy(source);
    return 0;
}
