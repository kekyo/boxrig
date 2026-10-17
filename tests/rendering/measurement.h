/* boxrig - Under MIT. */
#ifndef BOXRIG_TEST_RENDER_MEASUREMENT_H
#define BOXRIG_TEST_RENDER_MEASUREMENT_H
#include "../../src/measurement.h"
#include "boxrig/render/opengl.h"

typedef struct measured_stage {
    size_t calls;
    boxrig_measurement_time time;
} measured_stage;

extern bool measurement_detail;
extern measured_stage measurement_stages[BOXRIG_MEASURE_STAGE_COUNT];
extern boxrig_ogl_render_statistics measurement_gl_statistics;
extern size_t measurement_gl_draws;
extern size_t measurement_gtk_compositions;
extern uint64_t measurement_last_composition;
uint64_t measurement_clock(int clock_id);
void measurement_reset(void);
void measurement_print(void);
#endif
