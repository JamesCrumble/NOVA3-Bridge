#ifndef PORTBASE_FRAME_STATS_H
#define PORTBASE_FRAME_STATS_H

/*
 * What happened during the frame being stepped, for the one-line report of a frame that took far
 * longer than its neighbours (gl_bridge_profile_step). Counted where the work passes through the
 * port: file opens, uploads to the GPU, shader work, touch input. Reset at the start of each step;
 * plain fields, since a lost increment from another thread only blurs a diagnostic.
 */
struct PortFrameStats {
    unsigned opens, textures, shaders, links, touches, touch_age_ms;
    unsigned long tex_bytes, buf_bytes, vtx_bytes;
    char last_open[48];
};
extern PortFrameStats g_port_frame;

#endif
