#ifndef CFSK_PARTICLE_H
#define CFSK_PARTICLE_H

#include "cfsk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* @cfsk ForkBindingsCanvas.drawMeshInstances => cfsk_canvas_draw_mesh_instances | Graphite draws with retained mesh inputs and current records */
cfsk_status cfsk_canvas_draw_mesh_instances(cfsk_canvas* canvas, const cfsk_mesh* mesh,
        const cfsk_mesh_vertex_buffer* records, size_t count, cfsk_rect bounds,
        uint32_t blend_mode, const cfsk_paint* paint);

#ifdef __cplusplus
}
#endif

#endif
