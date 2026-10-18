/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * Interface between 'draw' module's output and the softpipe rasterizer/setup
 * code.  When the 'draw' module has finished filling a vertex buffer, the
 * draw_arrays() functions below will be called.  Loop over the vertices and
 * call the point/line/tri setup functions.
 *
 * Authors
 *  Brian Paul
 */


#include "sp_context.h"
#include "sp_setup.h"
#include "sp_state.h"
#include "sp_prim_vbuf.h"
#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#if DETECT_OS_NOCTURNE
#include "c11/threads.h"
#include "nocturne.h"
#include "sp_quad_pipe.h"
#include "sp_tex_sample.h"
#include "sp_tile_cache.h"
#endif


#if DETECT_OS_NOCTURNE
/* Bigger batches: each one is a hand-off to the rasterizer threads, and a
 * 4 KiB buffer held only a few dozen vertices. */
#define SP_MAX_VBUF_INDEXES 4096
#define SP_MAX_VBUF_SIZE    65536
#else
#define SP_MAX_VBUF_INDEXES 1024
#define SP_MAX_VBUF_SIZE    4096
#endif

typedef const float (*cptrf4)[4];

#if DETECT_OS_NOCTURNE
/* Fragment work is spread over the CPUs: every thread walks all triangles
 * of a draw but rasterizes only its own tile rows (sp_setup_set_band), so
 * per-pixel order is kept and no two threads touch the same tile. Each
 * thread runs on a shadow copy of the context with its own quad stages,
 * shader machine and samplers.
 *
 * Draws are queued rather than run one at a time: a draw becomes a job
 * holding a snapshot of the state, its vertices and indices and references
 * to its textures and constants, and the calling thread goes back to vertex
 * work while the threads take the jobs in order. Waiting for every thread
 * at the end of each draw left most of them idle, since a typical draw
 * covers only a few tile rows. Anything that reads or writes what queued
 * draws use first waits for the queue to empty (sp_raster_drain). */
#define SP_MAX_RASTER_THREADS 16
#define RASTER_QUEUE 128
/* about a millisecond: longer than the gap between consecutive draws */
#define RASTER_SPIN_CYCLES 3000000ull

struct softpipe_vbuf_render;

struct sp_raster_job
{
   /* State, written only when it differs from the previous job's: jobs
    * with the same serial share the snapshot of the first one. */
   unsigned serial;
   struct softpipe_context *state;
   unsigned nviews;
   struct sp_sampler *samplers[PIPE_MAX_SAMPLERS];
   struct sp_sampler_view *views;
   unsigned views_cap;
   void *consts[PIPE_MAX_CONSTANT_BUFFERS];   /* fragment constants */
   unsigned consts_cap[PIPE_MAX_CONSTANT_BUFFERS];

   /* the draw */
   enum mesa_prim prim;
   void *vertices;
   unsigned vertices_size;
   bool has_indices;
   uint16_t *indices;
   unsigned indices_size;
   uint nr;

   /* textures kept alive until the job is retired */
   struct pipe_resource *refs[PIPE_MAX_SHADER_SAMPLER_VIEWS];
   unsigned nrefs;
};

struct sp_raster_worker
{
   struct softpipe_vbuf_render *render;
   struct softpipe_context *shadow;
   struct tgsi_exec_machine *machine;
   struct sp_tgsi_sampler *sampler;
   struct setup_context *setup;
   unsigned bound_serial;
   bool bound;
   unsigned serial;                /* snapshot applied to the shadow */
   bool has_state;
   unsigned next;                  /* next job to run */
   unsigned done;                  /* jobs finished */
   thrd_t thread;
};
#endif

/**
 * Subclass of vbuf_render.
 */
struct softpipe_vbuf_render
{
   struct vbuf_render base;
   struct softpipe_context *softpipe;
   struct setup_context *setup;

   enum mesa_prim prim;
   uint vertex_size;
   uint nr_vertices;
   uint vertex_buffer_size;
   void *vertex_buffer;

#if DETECT_OS_NOCTURNE
   unsigned band_count;          /* 0: threads not started yet, 1: none */
   bool threads_inited;
   struct sp_raster_worker workers[SP_MAX_RASTER_THREADS];
   mtx_t lock;
   cnd_t start, done;
   /* Draws come back to back, and every blocking wait or wake is a system
    * call, so both sides spin briefly first and only sleep (or wake a
    * sleeper) when the other side is really idle. */
   unsigned sleepers;            /* workers blocked on start */
   bool caller_waiting;          /* caller blocked on done */
   unsigned wait_done;           /* until this many jobs are done */
   bool quit;
   struct sp_raster_job *jobs;   /* RASTER_QUEUE */
   unsigned head;                /* jobs queued */
   unsigned retired;             /* jobs whose references are released */
   unsigned serial;              /* last state snapshot */
   unsigned snapshot_slot;       /* job holding it */
   bool has_snapshot;
   bool caches_threaded;
#endif
};


/** cast wrapper */
static struct softpipe_vbuf_render *
softpipe_vbuf_render(struct vbuf_render *vbr)
{
   return (struct softpipe_vbuf_render *) vbr;
}


/** This tells the draw module about our desired vertex layout */
static const struct vertex_info *
sp_vbuf_get_vertex_info(struct vbuf_render *vbr)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   return softpipe_get_vbuf_vertex_info(cvbr->softpipe);
}


static bool
sp_vbuf_allocate_vertices(struct vbuf_render *vbr,
                          uint16_t vertex_size, uint16_t nr_vertices)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   unsigned size = vertex_size * nr_vertices;

   if (cvbr->vertex_buffer_size < size) {
      align_free(cvbr->vertex_buffer);
      cvbr->vertex_buffer = align_malloc(size, 16);
      cvbr->vertex_buffer_size = size;
   }

   cvbr->vertex_size = vertex_size;
   cvbr->nr_vertices = nr_vertices;
   
   return cvbr->vertex_buffer != NULL;
}


static void
sp_vbuf_release_vertices(struct vbuf_render *vbr)
{
   /* keep the old allocation for next time */
}


static void *
sp_vbuf_map_vertices(struct vbuf_render *vbr)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   return cvbr->vertex_buffer;
}


static void 
sp_vbuf_unmap_vertices(struct vbuf_render *vbr, 
                       uint16_t min_index,
                       uint16_t max_index )
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   assert( cvbr->vertex_buffer_size >= (max_index+1) * cvbr->vertex_size );
   (void) cvbr;
   /* do nothing */
}


static void
sp_vbuf_set_primitive(struct vbuf_render *vbr, enum mesa_prim prim)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   struct setup_context *setup_ctx = cvbr->setup;
   
   sp_setup_prepare( setup_ctx );

   cvbr->softpipe->reduced_prim = u_reduced_prim(prim);
   cvbr->prim = prim;
}


static inline cptrf4 get_vert( const void *vertex_buffer,
                               int index,
                               int stride )
{
   return (cptrf4)((char *)vertex_buffer + index * stride);
}


/**
 * draw elements / indexed primitives
 */
static void
rasterize_elements(const struct softpipe_context *softpipe, struct setup_context *setup,
                   enum mesa_prim prim, const void *vertex_buffer,
                   const uint16_t *indices, uint nr)
{
   const unsigned stride = softpipe->vertex_info.size * sizeof(float);
   const bool flatshade_first = softpipe->rasterizer->flatshade_first;
   unsigned i;

   switch (prim) {
   case MESA_PRIM_POINTS:
      for (i = 0; i < nr; i++) {
         sp_setup_point( setup,
                         get_vert(vertex_buffer, indices[i-0], stride) );
      }
      break;

   case MESA_PRIM_LINES:
      for (i = 1; i < nr; i += 2) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, indices[i-1], stride),
                        get_vert(vertex_buffer, indices[i-0], stride) );
      }
      break;

   case MESA_PRIM_LINE_STRIP:
      for (i = 1; i < nr; i ++) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, indices[i-1], stride),
                        get_vert(vertex_buffer, indices[i-0], stride) );
      }
      break;

   case MESA_PRIM_LINE_LOOP:
      for (i = 1; i < nr; i ++) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, indices[i-1], stride),
                        get_vert(vertex_buffer, indices[i-0], stride) );
      }
      if (nr) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, indices[nr-1], stride),
                        get_vert(vertex_buffer, indices[0], stride) );
      }
      break;

   case MESA_PRIM_TRIANGLES:
      for (i = 2; i < nr; i += 3) {
         sp_setup_tri( setup,
                       get_vert(vertex_buffer, indices[i-2], stride),
                       get_vert(vertex_buffer, indices[i-1], stride),
                       get_vert(vertex_buffer, indices[i-0], stride) );
      }
      break;

   case MESA_PRIM_TRIANGLE_STRIP:
      if (flatshade_first) {
         for (i = 2; i < nr; i += 1) {
            /* emit first triangle vertex as first triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-2], stride),
                          get_vert(vertex_buffer, indices[i+(i&1)-1], stride),
                          get_vert(vertex_buffer, indices[i-(i&1)], stride) );

         }
      }
      else {
         for (i = 2; i < nr; i += 1) {
            /* emit last triangle vertex as last triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i+(i&1)-2], stride),
                          get_vert(vertex_buffer, indices[i-(i&1)-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
         }
      }
      break;

   case MESA_PRIM_TRIANGLE_FAN:
      if (flatshade_first) {
         for (i = 2; i < nr; i += 1) {
            /* emit first non-spoke vertex as first vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[0], stride) );
         }
      }
      else {
         for (i = 2; i < nr; i += 1) {
            /* emit last non-spoke vertex as last vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[0], stride),
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
         }
      }
      break;

   case MESA_PRIM_QUADS:
      /* GL quads don't follow provoking vertex convention */
      if (flatshade_first) { 
         /* emit last quad vertex as first triangle vertex */
         for (i = 3; i < nr; i += 4) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[i-3], stride),
                          get_vert(vertex_buffer, indices[i-2], stride) );

            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[i-2], stride),
                          get_vert(vertex_buffer, indices[i-1], stride) );
         }
      }
      else {
         /* emit last quad vertex as last triangle vertex */
         for (i = 3; i < nr; i += 4) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-3], stride),
                          get_vert(vertex_buffer, indices[i-2], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );

            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-2], stride),
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
         }
      }
      break;

   case MESA_PRIM_QUAD_STRIP:
      /* GL quad strips don't follow provoking vertex convention */
      if (flatshade_first) { 
         /* emit last quad vertex as first triangle vertex */
         for (i = 3; i < nr; i += 2) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[i-3], stride),
                          get_vert(vertex_buffer, indices[i-2], stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-3], stride) );
         }
      }
      else {
         /* emit last quad vertex as last triangle vertex */
         for (i = 3; i < nr; i += 2) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-3], stride),
                          get_vert(vertex_buffer, indices[i-2], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-3], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
         }
      }
      break;

   case MESA_PRIM_POLYGON:
      /* Almost same as tri fan but the _first_ vertex specifies the flat
       * shading color.
       */
      if (flatshade_first) { 
         /* emit first polygon  vertex as first triangle vertex */
         for (i = 2; i < nr; i += 1) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[0], stride),
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride) );
         }
      }
      else {
         /* emit first polygon  vertex as last triangle vertex */
         for (i = 2; i < nr; i += 1) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, indices[i-1], stride),
                          get_vert(vertex_buffer, indices[i-0], stride),
                          get_vert(vertex_buffer, indices[0], stride) );
         }
      }
      break;

   default:
      assert(0);
   }
}


/**
 * This function is hit when the draw module is working in pass-through mode.
 * It's up to us to convert the vertex array into point/line/tri prims.
 */
static void
rasterize_arrays(const struct softpipe_context *softpipe, struct setup_context *setup,
                 enum mesa_prim prim, const void *vertices, uint start, uint nr)
{
   const unsigned stride = softpipe->vertex_info.size * sizeof(float);
   const void *vertex_buffer = (const void *) get_vert(vertices, start, stride);
   const bool flatshade_first = softpipe->rasterizer->flatshade_first;
   unsigned i;

   switch (prim) {
   case MESA_PRIM_POINTS:
      for (i = 0; i < nr; i++) {
         sp_setup_point( setup,
                         get_vert(vertex_buffer, i-0, stride) );
      }
      break;

   case MESA_PRIM_LINES:
      for (i = 1; i < nr; i += 2) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, i-1, stride),
                        get_vert(vertex_buffer, i-0, stride) );
      }
      break;

   case MESA_PRIM_LINES_ADJACENCY:
      for (i = 3; i < nr; i += 4) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, i-2, stride),
                        get_vert(vertex_buffer, i-1, stride) );
      }
      break;

   case MESA_PRIM_LINE_STRIP:
      for (i = 1; i < nr; i ++) {
         sp_setup_line( setup,
                     get_vert(vertex_buffer, i-1, stride),
                     get_vert(vertex_buffer, i-0, stride) );
      }
      break;

   case MESA_PRIM_LINE_STRIP_ADJACENCY:
      for (i = 3; i < nr; i++) {
         sp_setup_line( setup,
                     get_vert(vertex_buffer, i-2, stride),
                     get_vert(vertex_buffer, i-1, stride) );
      }
      break;

   case MESA_PRIM_LINE_LOOP:
      for (i = 1; i < nr; i ++) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, i-1, stride),
                        get_vert(vertex_buffer, i-0, stride) );
      }
      if (nr) {
         sp_setup_line( setup,
                        get_vert(vertex_buffer, nr-1, stride),
                        get_vert(vertex_buffer, 0, stride) );
      }
      break;

   case MESA_PRIM_TRIANGLES:
      for (i = 2; i < nr; i += 3) {
         sp_setup_tri( setup,
                       get_vert(vertex_buffer, i-2, stride),
                       get_vert(vertex_buffer, i-1, stride),
                       get_vert(vertex_buffer, i-0, stride) );
      }
      break;

   case MESA_PRIM_TRIANGLES_ADJACENCY:
      for (i = 5; i < nr; i += 6) {
         sp_setup_tri( setup,
                       get_vert(vertex_buffer, i-5, stride),
                       get_vert(vertex_buffer, i-3, stride),
                       get_vert(vertex_buffer, i-1, stride) );
      }
      break;

   case MESA_PRIM_TRIANGLE_STRIP:
      if (flatshade_first) {
         for (i = 2; i < nr; i++) {
            /* emit first triangle vertex as first triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-2, stride),
                          get_vert(vertex_buffer, i+(i&1)-1, stride),
                          get_vert(vertex_buffer, i-(i&1), stride) );
         }
      }
      else {
         for (i = 2; i < nr; i++) {
            /* emit last triangle vertex as last triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i+(i&1)-2, stride),
                          get_vert(vertex_buffer, i-(i&1)-1, stride),
                          get_vert(vertex_buffer, i-0, stride) );
         }
      }
      break;

   case MESA_PRIM_TRIANGLE_STRIP_ADJACENCY:
      if (flatshade_first) {
         for (i = 5; i < nr; i += 2) {
            /* emit first triangle vertex as first triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-5, stride),
                          get_vert(vertex_buffer, i+(i&1)*2-3, stride),
                          get_vert(vertex_buffer, i-(i&1)*2-1, stride) );
         }
      }
      else {
         for (i = 5; i < nr; i += 2) {
            /* emit last triangle vertex as last triangle vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i+(i&1)*2-5, stride),
                          get_vert(vertex_buffer, i-(i&1)*2-3, stride),
                          get_vert(vertex_buffer, i-1, stride) );
         }
      }
      break;

   case MESA_PRIM_TRIANGLE_FAN:
      if (flatshade_first) {
         for (i = 2; i < nr; i += 1) {
            /* emit first non-spoke vertex as first vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, 0, stride)  );
         }
      }
      else {
         for (i = 2; i < nr; i += 1) {
            /* emit last non-spoke vertex as last vertex */
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, 0, stride),
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-0, stride) );
         }
      }
      break;

   case MESA_PRIM_QUADS:
      /* GL quads don't follow provoking vertex convention */
      if (flatshade_first) { 
         /* emit last quad vertex as first triangle vertex */
         for (i = 3; i < nr; i += 4) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, i-3, stride),
                          get_vert(vertex_buffer, i-2, stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, i-2, stride),
                          get_vert(vertex_buffer, i-1, stride) );
         }
      }
      else {
         /* emit last quad vertex as last triangle vertex */
         for (i = 3; i < nr; i += 4) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-3, stride),
                          get_vert(vertex_buffer, i-2, stride),
                          get_vert(vertex_buffer, i-0, stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-2, stride),
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-0, stride) );
         }
      }
      break;

   case MESA_PRIM_QUAD_STRIP:
      /* GL quad strips don't follow provoking vertex convention */
      if (flatshade_first) { 
         /* emit last quad vertex as first triangle vertex */
         for (i = 3; i < nr; i += 2) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, i-3, stride),
                          get_vert(vertex_buffer, i-2, stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-3, stride) );
         }
      }
      else {
         /* emit last quad vertex as last triangle vertex */
         for (i = 3; i < nr; i += 2) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-3, stride),
                          get_vert(vertex_buffer, i-2, stride),
                          get_vert(vertex_buffer, i-0, stride) );
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-3, stride),
                          get_vert(vertex_buffer, i-0, stride) );
         }
      }
      break;

   case MESA_PRIM_POLYGON:
      /* Almost same as tri fan but the _first_ vertex specifies the flat
       * shading color.
       */
      if (flatshade_first) { 
         /* emit first polygon  vertex as first triangle vertex */
         for (i = 2; i < nr; i += 1) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, 0, stride),
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-0, stride) );
         }
      }
      else {
         /* emit first polygon  vertex as last triangle vertex */
         for (i = 2; i < nr; i += 1) {
            sp_setup_tri( setup,
                          get_vert(vertex_buffer, i-1, stride),
                          get_vert(vertex_buffer, i-0, stride),
                          get_vert(vertex_buffer, 0, stride) );
         }
      }
      break;

   default:
      assert(0);
   }
}


#if DETECT_OS_NOCTURNE
/* Copies the context state the rasterizer reads into a shadow context. The
 * resource binding tables (samplers, sampler views, images, buffers, vertex
 * buffers, texture caches) are tens of kilobytes; threaded draws read
 * textures only through the worker's own sampler copy and never use images,
 * buffers or vertex inputs, so those ranges are left out. */
#define SHADOW_RANGE(dst, src, from, to) \
   memcpy((char *)(dst) + offsetof(struct softpipe_context, from), \
          (const char *)(src) + offsetof(struct softpipe_context, from), \
          offsetof(struct softpipe_context, to) - offsetof(struct softpipe_context, from))
#define SAME_RANGE(a, b, from, to) \
   (!memcmp((const char *)(a) + offsetof(struct softpipe_context, from), \
            (const char *)(b) + offsetof(struct softpipe_context, from), \
            offsetof(struct softpipe_context, to) - offsetof(struct softpipe_context, from)))

static void
raster_shadow_copy(struct softpipe_context *shadow, const struct softpipe_context *sp)
{
   memcpy(shadow, sp, offsetof(struct softpipe_context, samplers));
   SHADOW_RANGE(shadow, sp, depth_stencil, constants);
   SHADOW_RANGE(shadow, sp, framebuffer, sampler_views);
   SHADOW_RANGE(shadow, sp, viewports, vertex_buffer);
   SHADOW_RANGE(shadow, sp, so_targets, mapped_vbuffer);
   SHADOW_RANGE(shadow, sp, mapped_constants, tex_cache);
}


/* Whether the context still has the state of the last snapshot. The other
 * stages' constants change with nearly every draw and are left out; the
 * fragment constants are compared by content, since the frontend rewrites
 * the same user buffer in place. */
static bool
raster_snapshot_current(struct softpipe_vbuf_render *cvbr, unsigned nviews)
{
   const struct softpipe_context *sp = cvbr->softpipe;
   const struct sp_tgsi_sampler *samp = sp->tgsi.sampler[MESA_SHADER_FRAGMENT];
   const struct sp_raster_job *snap = &cvbr->jobs[cvbr->snapshot_slot];
   const struct softpipe_context *state = snap->state;
   unsigned i;

   if (!cvbr->has_snapshot || snap->nviews != nviews ||
       !SAME_RANGE(sp, state, blend, samplers) ||
       !SAME_RANGE(sp, state, depth_stencil, constants) ||
       !SAME_RANGE(sp, state, framebuffer, sampler_views) ||
       !SAME_RANGE(sp, state, viewports, vertex_buffer) ||
       !SAME_RANGE(sp, state, so_targets, mapped_vbuffer) ||
       !SAME_RANGE(sp, state, setup_info, tex_cache))
      return false;
   for (i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct tgsi_exec_consts_info *a = &sp->mapped_constants[MESA_SHADER_FRAGMENT][i];
      const struct tgsi_exec_consts_info *b = &state->mapped_constants[MESA_SHADER_FRAGMENT][i];

      if (a->size != b->size || !a->ptr != !b->ptr ||
          (a->ptr && memcmp(a->ptr, b->ptr, a->size)))
         return false;
   }
   return !memcmp(snap->samplers, samp->sp_sampler,
                  MIN2(nviews, PIPE_MAX_SAMPLERS) * sizeof(samp->sp_sampler[0])) &&
          !memcmp(snap->views, samp->sp_sview, nviews * sizeof(samp->sp_sview[0]));
}


/* Takes a snapshot of the context into the job. The fragment constants are
 * copied with it. */
static bool
raster_snapshot_take(struct softpipe_vbuf_render *cvbr, struct sp_raster_job *job,
                     unsigned nviews)
{
   const struct softpipe_context *sp = cvbr->softpipe;
   const struct sp_tgsi_sampler *samp = sp->tgsi.sampler[MESA_SHADER_FRAGMENT];
   unsigned i;

   if (!job->state && !(job->state = CALLOC_STRUCT(softpipe_context)))
      return false;
   if (job->views_cap < nviews) {
      FREE(job->views);
      job->views = MALLOC(nviews * sizeof(job->views[0]));
      job->views_cap = job->views ? nviews : 0;
      if (!job->views)
         return false;
   }
   for (i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct tgsi_exec_consts_info *c = &sp->mapped_constants[MESA_SHADER_FRAGMENT][i];

      if (c->ptr && job->consts_cap[i] < c->size) {
         FREE(job->consts[i]);
         job->consts[i] = MALLOC(c->size);
         job->consts_cap[i] = job->consts[i] ? c->size : 0;
         if (!job->consts[i])
            return false;
      }
   }

   raster_shadow_copy(job->state, sp);
   for (i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct tgsi_exec_consts_info *c = &sp->mapped_constants[MESA_SHADER_FRAGMENT][i];

      if (c->ptr) {
         memcpy(job->consts[i], c->ptr, c->size);
         job->state->mapped_constants[MESA_SHADER_FRAGMENT][i].ptr = job->consts[i];
      }
   }
   job->nviews = nviews;
   memcpy(job->samplers, samp->sp_sampler,
          MIN2(nviews, PIPE_MAX_SAMPLERS) * sizeof(samp->sp_sampler[0]));
   memcpy(job->views, samp->sp_sview, nviews * sizeof(samp->sp_sview[0]));

   cvbr->serial++;
   cvbr->snapshot_slot = job - cvbr->jobs;
   cvbr->has_snapshot = true;
   return true;
}
#undef SAME_RANGE
#undef SHADOW_RANGE


/* Moves a job's state snapshot into the worker's shadow context. */
static void
raster_apply_state(struct sp_raster_worker *w, const struct sp_raster_job *job)
{
   struct softpipe_context *shadow = w->shadow;
   struct quad_stage *shade = shadow->quad.shade;
   struct quad_stage *depth_test = shadow->quad.depth_test;
   struct quad_stage *blend = shadow->quad.blend;

   raster_shadow_copy(shadow, job->state);
   shadow->quad.shade = shade;
   shadow->quad.depth_test = depth_test;
   shadow->quad.blend = blend;
   shadow->fs_machine = w->machine;
   shadow->tgsi.sampler[MESA_SHADER_FRAGMENT] = w->sampler;
   shadow->draw = NULL;
   shadow->dirty = 0;

   memcpy(w->sampler->sp_sampler, job->samplers,
          MIN2(job->nviews, PIPE_MAX_SAMPLERS) * sizeof(job->samplers[0]));
   memcpy(w->sampler->sp_sview, job->views, job->nviews * sizeof(job->views[0]));

   if (!w->bound || w->bound_serial != shadow->fs_bind_serial) {
      shadow->fs_variant->prepare(shadow->fs_variant, w->machine,
                                  (struct tgsi_sampler *)w->sampler,
                                  (struct tgsi_image *)shadow->tgsi.image[MESA_SHADER_FRAGMENT],
                                  (struct tgsi_buffer *)shadow->tgsi.buffer[MESA_SHADER_FRAGMENT]);
      w->bound_serial = shadow->fs_bind_serial;
      w->bound = true;
   }

   sp_build_quad_pipeline(shadow);
   sp_setup_prepare(w->setup);
   w->serial = job->serial;
   w->has_state = true;
}


static void
raster_job_run(struct sp_raster_worker *w, const struct sp_raster_job *job)
{
   if (!w->has_state || w->serial != job->serial)
      raster_apply_state(w, job);
   if (job->has_indices)
      rasterize_elements(w->shadow, w->setup, job->prim, job->vertices,
                         job->indices, job->nr);
   else
      rasterize_arrays(w->shadow, w->setup, job->prim, job->vertices, 0, job->nr);
}


static unsigned
raster_jobs_done(struct softpipe_vbuf_render *cvbr);

static int
raster_thread(void *arg)
{
   struct sp_raster_worker *w = arg;
   struct softpipe_vbuf_render *cvbr = w->render;

   for (;;) {
      const uint64_t until = __builtin_ia32_rdtsc() + RASTER_SPIN_CYCLES;
      unsigned head;

      while ((head = __atomic_load_n(&cvbr->head, __ATOMIC_SEQ_CST)) == w->next &&
             !__atomic_load_n(&cvbr->quit, __ATOMIC_SEQ_CST) &&
             __builtin_ia32_rdtsc() < until)
         __builtin_ia32_pause();
      if (head == w->next) {
         mtx_lock(&cvbr->lock);
         __atomic_add_fetch(&cvbr->sleepers, 1, __ATOMIC_SEQ_CST);
         while ((head = __atomic_load_n(&cvbr->head, __ATOMIC_SEQ_CST)) == w->next &&
                !__atomic_load_n(&cvbr->quit, __ATOMIC_SEQ_CST))
            cnd_wait(&cvbr->start, &cvbr->lock);
         __atomic_sub_fetch(&cvbr->sleepers, 1, __ATOMIC_SEQ_CST);
         mtx_unlock(&cvbr->lock);
      }
      /* the queue is empty before quit is set */
      if (head == w->next)
         break;

      while (w->next != head) {
         raster_job_run(w, &cvbr->jobs[w->next % RASTER_QUEUE]);
         w->next++;
         __atomic_store_n(&w->done, w->next, __ATOMIC_SEQ_CST);
         /* a wake is a system call: only the thread that completes what
          * the caller waits for makes it */
         if (__atomic_load_n(&cvbr->caller_waiting, __ATOMIC_SEQ_CST) &&
             (int)(raster_jobs_done(cvbr) -
                   __atomic_load_n(&cvbr->wait_done, __ATOMIC_SEQ_CST)) >= 0) {
            mtx_lock(&cvbr->lock);
            cnd_signal(&cvbr->done);
            mtx_unlock(&cvbr->lock);
         }
      }
   }
   return 0;
}


static void
raster_worker_free(struct sp_raster_worker *w)
{
   if (w->setup)
      sp_setup_destroy_context(w->setup);
   if (w->shadow) {
      if (w->shadow->quad.shade)
         w->shadow->quad.shade->destroy(w->shadow->quad.shade);
      if (w->shadow->quad.depth_test)
         w->shadow->quad.depth_test->destroy(w->shadow->quad.depth_test);
      if (w->shadow->quad.blend)
         w->shadow->quad.blend->destroy(w->shadow->quad.blend);
      FREE(w->shadow);
   }
   if (w->machine)
      tgsi_exec_machine_destroy(w->machine);
   FREE(w->sampler);
   memset(w, 0, sizeof(*w));
}


static bool
raster_worker_init(struct softpipe_vbuf_render *cvbr, struct sp_raster_worker *w)
{
   w->render = cvbr;
   w->shadow = CALLOC_STRUCT(softpipe_context);
   w->machine = tgsi_exec_machine_create(MESA_SHADER_FRAGMENT);
   w->sampler = sp_create_tgsi_sampler();
   if (!w->shadow || !w->machine || !w->sampler)
      return false;
   w->shadow->quad.shade = sp_quad_shade_stage(w->shadow);
   w->shadow->quad.depth_test = sp_quad_depth_test_stage(w->shadow);
   w->shadow->quad.blend = sp_quad_blend_stage(w->shadow);
   w->setup = sp_setup_create_context(w->shadow);
   if (!w->shadow->quad.shade || !w->shadow->quad.depth_test ||
       !w->shadow->quad.blend || !w->setup)
      return false;
   return thrd_create(&w->thread, raster_thread, w) == thrd_success;
}


static void
raster_threads_stop(struct softpipe_vbuf_render *cvbr);

static void
raster_threads_start(struct softpipe_vbuf_render *cvbr)
{
   struct n_cpuinfo cpu = {0};
   unsigned count = cpu_info(&cpu) >= 0 && cpu.online_cpus ? cpu.online_cpus : 1;
   unsigned started = 0, i;

   cvbr->band_count = 1;
   count = MIN2(count, SP_MAX_RASTER_THREADS);
   if (count < 2)
      return;
   cvbr->jobs = CALLOC(RASTER_QUEUE, sizeof(cvbr->jobs[0]));
   if (!cvbr->jobs)
      return;
   if (mtx_init(&cvbr->lock, mtx_plain) != thrd_success)
      goto fail_jobs;
   if (cnd_init(&cvbr->start) != thrd_success)
      goto fail_lock;
   if (cnd_init(&cvbr->done) != thrd_success)
      goto fail_start;

   for (i = 0; i < count; i++) {
      if (!raster_worker_init(cvbr, &cvbr->workers[i])) {
         raster_worker_free(&cvbr->workers[i]);
         break;
      }
      started++;
   }
   cvbr->band_count = started;
   cvbr->threads_inited = true;
   if (started < 2) {
      raster_threads_stop(cvbr);
      cvbr->band_count = 1;
      return;
   }
   /* Bands are fixed once the threads exist; each worker reads its setup
    * context only when jobs come in. */
   for (i = 0; i < started; i++)
      sp_setup_set_band(cvbr->workers[i].setup, i, started);
   return;

fail_start:
   cnd_destroy(&cvbr->start);
fail_lock:
   mtx_destroy(&cvbr->lock);
fail_jobs:
   FREE(cvbr->jobs);
   cvbr->jobs = NULL;
}


/* Jobs every thread has finished. */
static unsigned
raster_jobs_done(struct softpipe_vbuf_render *cvbr)
{
   unsigned done = __atomic_load_n(&cvbr->head, __ATOMIC_SEQ_CST), i;

   for (i = 0; i < cvbr->band_count; i++) {
      unsigned d = __atomic_load_n(&cvbr->workers[i].done, __ATOMIC_SEQ_CST);

      if ((int)(d - done) < 0)
         done = d;
   }
   return done;
}


/* Releases the references of finished jobs; this stays on the calling
 * thread, since dropping the last one destroys the resource. */
static void
raster_jobs_retire(struct softpipe_vbuf_render *cvbr, unsigned done)
{
   while (cvbr->retired != done) {
      struct sp_raster_job *job = &cvbr->jobs[cvbr->retired % RASTER_QUEUE];
      unsigned i;

      for (i = 0; i < job->nrefs; i++)
         pipe_resource_reference(&job->refs[i], NULL);
      job->nrefs = 0;
      cvbr->retired++;
   }
}


/* Waits until at most `limit` jobs are left. */
static void
raster_jobs_wait(struct softpipe_vbuf_render *cvbr, unsigned limit)
{
   unsigned done = raster_jobs_done(cvbr);

   if (cvbr->head - done > limit) {
      const uint64_t until = __builtin_ia32_rdtsc() + RASTER_SPIN_CYCLES;

      while ((done = raster_jobs_done(cvbr), cvbr->head - done > limit) &&
             __builtin_ia32_rdtsc() < until)
         __builtin_ia32_pause();
      if (cvbr->head - done > limit) {
         mtx_lock(&cvbr->lock);
         __atomic_store_n(&cvbr->wait_done, cvbr->head - limit, __ATOMIC_SEQ_CST);
         __atomic_store_n(&cvbr->caller_waiting, true, __ATOMIC_SEQ_CST);
         while ((done = raster_jobs_done(cvbr), cvbr->head - done > limit))
            cnd_wait(&cvbr->done, &cvbr->lock);
         __atomic_store_n(&cvbr->caller_waiting, false, __ATOMIC_SEQ_CST);
         mtx_unlock(&cvbr->lock);
      }
   }
   raster_jobs_retire(cvbr, done);
}


static void
raster_caches_threaded(struct softpipe_context *sp, bool threaded)
{
   unsigned i;

   for (i = 0; i < sp->framebuffer.nr_cbufs; i++) {
      if (sp->cbuf_cache[i]) {
         sp->cbuf_cache[i]->threaded = threaded;
         sp->cbuf_cache[i]->last_tile_addr.bits.invalid = 1;
      }
   }
   if (sp->zsbuf_cache) {
      sp->zsbuf_cache->threaded = threaded;
      sp->zsbuf_cache->last_tile_addr.bits.invalid = 1;
   }
}


/* Waits for every queued draw and hands the tile caches back. */
static void
raster_drain(struct softpipe_vbuf_render *cvbr)
{
   if (!cvbr->threads_inited)
      return;
   if (cvbr->retired != cvbr->head)
      raster_jobs_wait(cvbr, 0);
   if (cvbr->caches_threaded) {
      raster_caches_threaded(cvbr->softpipe, false);
      cvbr->caches_threaded = false;
   }
}


void
sp_raster_drain(struct softpipe_context *softpipe)
{
   if (softpipe->vbuf_backend)
      raster_drain(softpipe_vbuf_render(softpipe->vbuf_backend));
}


static void
raster_threads_stop(struct softpipe_vbuf_render *cvbr)
{
   unsigned i, j;

   if (!cvbr->threads_inited)
      return;
   raster_drain(cvbr);
   mtx_lock(&cvbr->lock);
   __atomic_store_n(&cvbr->quit, true, __ATOMIC_SEQ_CST);
   cnd_broadcast(&cvbr->start);
   mtx_unlock(&cvbr->lock);
   for (i = 0; i < cvbr->band_count; i++) {
      thrd_join(cvbr->workers[i].thread, NULL);
      raster_worker_free(&cvbr->workers[i]);
   }
   for (i = 0; i < RASTER_QUEUE; i++) {
      struct sp_raster_job *job = &cvbr->jobs[i];

      FREE(job->state);
      FREE(job->views);
      FREE(job->vertices);
      FREE(job->indices);
      for (j = 0; j < PIPE_MAX_CONSTANT_BUFFERS; j++)
         FREE(job->consts[j]);
   }
   FREE(cvbr->jobs);
   cvbr->jobs = NULL;
   cnd_destroy(&cvbr->done);
   cnd_destroy(&cvbr->start);
   mtx_destroy(&cvbr->lock);
   cvbr->threads_inited = false;
}


/**
 * Whether this draw can be split over the threads. Returns the number of
 * fragment sampler views the shadow contexts need in *nviews.
 */
static bool
raster_threads_usable(struct softpipe_vbuf_render *cvbr, unsigned *nviews)
{
   struct softpipe_context *sp = cvbr->softpipe;
   const struct sp_fragment_shader_variant *fs = sp->fs_variant;
   const struct sp_tgsi_sampler *samp = sp->tgsi.sampler[MESA_SHADER_FRAGMENT];
   unsigned n, i;

   if (cvbr->band_count < 2 || !fs || sp->reduced_prim != MESA_PRIM_TRIANGLES)
      return false;
   /* counters shared by all fragments, and layered rendering, whose tiles
    * are not covered by the collision-free cache placement */
   if (sp->active_statistics_queries || sp->active_query_count ||
       sp->layer_slot > 0)
      return false;
   /* shader side effects keep their single-threaded order */
   if (fs->info.file_count[TGSI_FILE_IMAGE] ||
       fs->info.file_count[TGSI_FILE_BUFFER] ||
       fs->info.file_count[TGSI_FILE_MEMORY])
      return false;
   /* tiles of a framebuffer within the cache span never share an entry */
   if (sp->framebuffer.width > SP_TILE_CACHE_SPAN * TILE_SIZE ||
       sp->framebuffer.height > SP_TILE_CACHE_SPAN * TILE_SIZE)
      return false;

   n = MAX3(sp->num_sampler_views[MESA_SHADER_FRAGMENT],
            (unsigned)(fs->info.file_max[TGSI_FILE_SAMPLER_VIEW] + 1),
            (unsigned)(fs->info.file_max[TGSI_FILE_SAMPLER] + 1));
   n = MIN2(n, PIPE_MAX_SHADER_SAMPLER_VIEWS);
   /* textures read through the shared texture tile cache stay on one thread */
   for (i = 0; i < n; i++) {
      if (samp->sp_sview[i].base.texture && !samp->sp_sview[i].direct)
         return false;
   }
   *nviews = n;
   return true;
}


/* Makes room for a copy of `size` bytes in a job buffer. */
static bool
raster_job_reserve(void **buf, unsigned *cap, size_t size)
{
   if (*cap >= size)
      return true;
   FREE(*buf);
   size = MAX2(size, 4096);
   *buf = MALLOC(size);
   *cap = *buf ? size : 0;
   return *buf != NULL;
}


/**
 * Queue the current draw for the threads. Returns false when the draw has
 * to be rasterized on the calling thread; the queue is empty then.
 */
static bool
raster_threads_run(struct softpipe_vbuf_render *cvbr,
                   const uint16_t *indices, uint start, uint nr)
{
   struct softpipe_context *sp = cvbr->softpipe;
   const struct sp_tgsi_sampler *samp = sp->tgsi.sampler[MESA_SHADER_FRAGMENT];
   struct sp_raster_job *job;
   unsigned nviews, done, count, i;

   if (!cvbr->band_count)
      raster_threads_start(cvbr);
   if (!raster_threads_usable(cvbr, &nviews)) {
      raster_drain(cvbr);
      return false;
   }

   /* a free slot; when the queue is full, let it run half empty so the
    * wait is not paid for every draw */
   done = raster_jobs_done(cvbr);
   if (cvbr->head - done >= RASTER_QUEUE)
      raster_jobs_wait(cvbr, RASTER_QUEUE / 2);
   else
      raster_jobs_retire(cvbr, done);
   job = &cvbr->jobs[cvbr->head % RASTER_QUEUE];

   if (indices) {
      unsigned max = 0;

      for (i = 0; i < nr; i++)
         max = MAX2(max, indices[i]);
      count = nr ? max + 1 : 0;
   } else {
      count = nr;
   }
   if (!raster_job_reserve(&job->vertices, &job->vertices_size,
                           (size_t)count * cvbr->vertex_size) ||
       (indices && !raster_job_reserve((void **)&job->indices, &job->indices_size,
                                       (size_t)nr * sizeof(*indices))) ||
       (!raster_snapshot_current(cvbr, nviews) &&
        !raster_snapshot_take(cvbr, job, nviews))) {
      /* the snapshot in this slot may be half written */
      if (cvbr->has_snapshot && cvbr->snapshot_slot == job - cvbr->jobs)
         cvbr->has_snapshot = false;
      raster_drain(cvbr);
      return false;
   }

   job->serial = cvbr->serial;
   job->prim = cvbr->prim;
   job->nr = nr;
   memcpy(job->vertices,
          (const char *)cvbr->vertex_buffer + (size_t)(indices ? 0 : start) * cvbr->vertex_size,
          (size_t)count * cvbr->vertex_size);
   if (indices)
      memcpy(job->indices, indices, (size_t)nr * sizeof(*indices));
   job->has_indices = indices != NULL;
   for (i = 0; i < nviews; i++) {
      if (samp->sp_sview[i].base.texture)
         pipe_resource_reference(&job->refs[job->nrefs++], samp->sp_sview[i].base.texture);
   }

   if (!cvbr->caches_threaded) {
      raster_caches_threaded(sp, true);
      cvbr->caches_threaded = true;
   }
   __atomic_add_fetch(&cvbr->head, 1, __ATOMIC_SEQ_CST);
   if (__atomic_load_n(&cvbr->sleepers, __ATOMIC_SEQ_CST)) {
      mtx_lock(&cvbr->lock);
      cnd_broadcast(&cvbr->start);
      mtx_unlock(&cvbr->lock);
   }
   return true;
}
#endif


static void
sp_vbuf_draw_elements(struct vbuf_render *vbr, const uint16_t *indices, uint nr)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);

#if DETECT_OS_NOCTURNE
   if (raster_threads_run(cvbr, indices, 0, nr))
      return;
#endif
   rasterize_elements(cvbr->softpipe, cvbr->setup, cvbr->prim,
                      cvbr->vertex_buffer, indices, nr);
}


static void
sp_vbuf_draw_arrays(struct vbuf_render *vbr, uint start, uint nr)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);

#if DETECT_OS_NOCTURNE
   if (raster_threads_run(cvbr, NULL, start, nr))
      return;
#endif
   rasterize_arrays(cvbr->softpipe, cvbr->setup, cvbr->prim,
                    cvbr->vertex_buffer, start, nr);
}

/*
 * FIXME: it is unclear if primitives_storage_needed (which is generally
 * the same as pipe query num_primitives_generated) should increase
 * if SO is disabled for d3d10, but for GL we definitely need to
 * increase num_primitives_generated and this is only called for active
 * SO. If it must not increase for d3d10 need to disambiguate the counters
 * in the driver and do some work for getting correct values, if it should
 * increase too should call this from outside streamout code.
 */
static void
sp_vbuf_so_info(struct vbuf_render *vbr, uint stream, uint primitives, uint prim_generated)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   struct softpipe_context *softpipe = cvbr->softpipe;

   softpipe->so_stats[stream].num_primitives_written += primitives;
   softpipe->so_stats[stream].primitives_storage_needed += prim_generated;
}

static void
sp_vbuf_pipeline_statistics(
   struct vbuf_render *vbr, 
   const struct pipe_query_data_pipeline_statistics *stats)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
   struct softpipe_context *softpipe = cvbr->softpipe;

   softpipe->pipeline_statistics.ia_vertices +=
      stats->ia_vertices;
   softpipe->pipeline_statistics.ia_primitives +=
      stats->ia_primitives;
   softpipe->pipeline_statistics.vs_invocations +=
      stats->vs_invocations;
   softpipe->pipeline_statistics.gs_invocations +=
      stats->gs_invocations;
   softpipe->pipeline_statistics.gs_primitives +=
      stats->gs_primitives;
   softpipe->pipeline_statistics.c_invocations +=
      stats->c_invocations;
}


static void
sp_vbuf_destroy(struct vbuf_render *vbr)
{
   struct softpipe_vbuf_render *cvbr = softpipe_vbuf_render(vbr);
#if DETECT_OS_NOCTURNE
   raster_threads_stop(cvbr);
   if (cvbr->softpipe->vbuf_backend == vbr)
      cvbr->softpipe->vbuf_backend = NULL;
#endif
   if (cvbr->vertex_buffer)
      align_free(cvbr->vertex_buffer);
   sp_setup_destroy_context(cvbr->setup);
   FREE(cvbr);
}


/**
 * Create the post-transform vertex handler for the given context.
 */
struct vbuf_render *
sp_create_vbuf_backend(struct softpipe_context *sp)
{
   struct softpipe_vbuf_render *cvbr = CALLOC_STRUCT(softpipe_vbuf_render);

   assert(sp->draw);

   cvbr->base.max_indices = SP_MAX_VBUF_INDEXES;
   cvbr->base.max_vertex_buffer_bytes = SP_MAX_VBUF_SIZE;

   cvbr->base.get_vertex_info = sp_vbuf_get_vertex_info;
   cvbr->base.allocate_vertices = sp_vbuf_allocate_vertices;
   cvbr->base.map_vertices = sp_vbuf_map_vertices;
   cvbr->base.unmap_vertices = sp_vbuf_unmap_vertices;
   cvbr->base.set_primitive = sp_vbuf_set_primitive;
   cvbr->base.draw_elements = sp_vbuf_draw_elements;
   cvbr->base.draw_arrays = sp_vbuf_draw_arrays;
   cvbr->base.release_vertices = sp_vbuf_release_vertices;
   cvbr->base.set_stream_output_info = sp_vbuf_so_info;
   cvbr->base.pipeline_statistics = sp_vbuf_pipeline_statistics;
   cvbr->base.destroy = sp_vbuf_destroy;

   cvbr->softpipe = sp;

   cvbr->setup = sp_setup_create_context(cvbr->softpipe);

   return &cvbr->base;
}
