/* boxrig - Under MIT. */
#ifndef BOXRIG_MEASUREMENT_H
#define BOXRIG_MEASUREMENT_H

/* Private diagnostic hooks. Ordinary libraries contain neither clocks nor
 * calls to these hooks; only the separate measurement build defines them. */
#ifdef BOXRIG_MEASURE_RENDERING
#include <stdint.h>
typedef struct boxrig_measurement_time {
    uint64_t elapsed;
    uint64_t cpu;
} boxrig_measurement_time;

typedef enum boxrig_measurement_stage {
    BOXRIG_MEASURE_FEED,
    BOXRIG_MEASURE_RECOGNITION,
    BOXRIG_MEASURE_SNAPSHOT,
    BOXRIG_MEASURE_ROW_SIGNATURE,
    BOXRIG_MEASURE_SHAPE,
    BOXRIG_MEASURE_CAIRO_RENDER,
    BOXRIG_MEASURE_CAIRO_ALLOCATE,
    BOXRIG_MEASURE_CAIRO_COPY,
    BOXRIG_MEASURE_FRAME,
    BOXRIG_MEASURE_GL_DRAW,
    BOXRIG_MEASURE_GL_PRESENT,
    BOXRIG_MEASURE_GTK_IMPORT,
    BOXRIG_MEASURE_GTK_COMPOSE,
    BOXRIG_MEASURE_TEXT_SYNC,
    BOXRIG_MEASURE_STAGE_COUNT
} boxrig_measurement_stage;

boxrig_measurement_time boxrig_measurement_begin(void);
void boxrig_measurement_end(boxrig_measurement_stage stage, boxrig_measurement_time start);
#define BOXRIG_MEASURE_BEGIN(name) boxrig_measurement_time name = boxrig_measurement_begin()
#define BOXRIG_MEASURE_END(stage, name) boxrig_measurement_end(stage, name)
#else
#define BOXRIG_MEASURE_BEGIN(name) ((void)0)
#define BOXRIG_MEASURE_END(stage, name) ((void)0)
#endif

#endif
