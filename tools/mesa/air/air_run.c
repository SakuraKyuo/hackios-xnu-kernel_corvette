/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * air_run: draw with a Metal shader and read the pixels back.
 *
 * This is the half of M1 that compiling cannot give. The shader comes out of a
 * .metallib, through air_to_nir, into a Gallium pipe_shader_state as
 * PIPE_SHADER_IR_NIR -- which both drivers of interest accept natively: ir3
 * is a NIR compiler, and softpipe takes NIR through nir_to_tgsi. One quad is
 * drawn into a 32-bit-float render target and one pixel is printed, so the
 * result is a number and not a picture.
 *
 * On this machine the screen is softpipe, so the whole path can be run with no
 * GPU at all. On the phone the same source wants the freedreno screen that
 * tools/mesa/freedreno-darwin.patch already creates inside the port's Mesa
 * (osmesa_create_gpu_screen), and nothing else about it changes.
 *
 * usage: air_run FILE[.metallib] [FUNCTION]
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_air.h"
#include "air_module.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"
#include "target-helpers/inline_sw_helper.h"
#include "null/null_sw_winsys.h"
#ifdef GALLIUM_FREEDRENO
#include "freedreno/drm/freedreno_drm_public.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include <xf86drm.h>
#endif
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <signal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <dlfcn.h>
#if defined(GALLIUM_FREEDRENO)
#include "frontend/winsys_handle.h"
#endif
/* <sys/ucontext.h>, not <ucontext.h>: the latter refuses to be included
 * without _XOPEN_SOURCE on the iOS SDK, and userland/src/inject.c takes the
 * same route for the same reason.
 */
#include <sys/ucontext.h>
#endif

#define WIDTH 4
#define HEIGHT 4

/* The texture bound for every sampled texture the shader declares: four by
 * four, 32-bit floats, each texel a number that says which texel it is. The
 * sampler is nearest with no mip filter and clamped addressing, and the
 * coordinate the vertex shader hands over is the centre of texel (1, 1), so
 * a correct sample is an exact equality and not a tolerance -- no filtering
 * weights, no wrap arithmetic, nothing to round.
 *
 * This verifies the texture operations whose sampler the API chooses (nine
 * modules of the corpus) and those that take no sampler at all (twenty-nine,
 * which fetch by integer coordinate). It cannot verify the two whose sampler
 * the shader declares itself: that needs Metal's 64-bit sampler word decoded,
 * which it is not.
 */
#define TEX_SIZE 4
#define TEX_X 1
#define TEX_Y 1

/* Slot is in the value on purpose. Binding one texture to every slot makes a
 * shader that reads the wrong slot return the right texel, which is a test
 * passing for the wrong reason -- the third time that shape of mistake has
 * turned up here, after the 2D texture bound for a 3D sampler and the colour
 * attachment nothing stored to. Looked for deliberately this time rather than
 * tripped over: slot n adds a thousand times n, so a misbinding is visible.
 */
static void
texel_value(unsigned slot, unsigned x, unsigned y, unsigned z, float out[4])
{
   out[0] = 100.0f + x + 1000.0f * slot;
   out[1] = 200.0f + y;
   out[2] = 300.0f + x + y + z;
   out[3] = 400.0f;
}

#ifdef __APPLE__
/*
 * A program that dies on this port leaves nothing behind: the kernel says
 * "Failed to send exception EXC_CORPSE_NOTIFY" and that is the end of it. So
 * print the fault before dying. These two functions are userland/src/inject.c's
 * say_the_fault()/watch_for_faults() (branch sileo2, 6683521), with the slide
 * taken from this program's own image rather than from the shared cache, since
 * the pc of interest is in air_run or in libOSMesa and not in iOS.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
   ucontext_t *context = context_in;
   uintptr_t slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
   uintptr_t pc = 0, lr = 0, fp = 0;

   if (context != NULL && context->uc_mcontext != NULL) {
      pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
      lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
      fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
   }
   /* who called, by the frame pointers: on arm64 each frame holds the previous
    * at [fp] and its return address at [fp + 8]
    */
   char frames[8 * 20 + 1] = "";
   size_t used = 0;
   for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
      uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
      if (ret == 0)
         break;
      used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
      fp = next > fp ? next : 0;   /* the chain only ever goes up the stack */
   }
   fprintf(stderr, "air_run: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n",
           signal_number, info != NULL ? info->si_addr : NULL, pc - slide, lr - slide,
           frames);
   fflush(stderr);
   signal(signal_number, SIG_DFL);
   raise(signal_number);
}

static void
watch_for_faults(void)
{
   struct sigaction action;

   memset(&action, 0, sizeof(action));
   action.sa_sigaction = say_the_fault;
   action.sa_flags = SA_SIGINFO | SA_RESETHAND;
   sigaction(SIGSEGV, &action, NULL);
   sigaction(SIGBUS, &action, NULL);
}
#else
static void watch_for_faults(void) {}
#endif

#ifdef __APPLE__
/*
 * --panel: the picture on the phone's panel.
 *
 * The framebuffer (iokit/Kernel/MiniIOMobileFramebuffer.cpp) shows a surface a swap names, once the
 * GPU is done with it: present() waits on the fence of the submission that named the surface's
 * pixels and show() copies the surface onto the panel. The window server's frames go that way
 * every day; a test program puts its own picture there the same way -- an IOSurface as the render
 * target (the memory the window server's layers are made of), the GPU told to draw into it, and
 * SwapBegin/SwapEnd with that surface.
 *
 * The request's layout is the framebuffer's (its SWAP_* constants, worked out from the iOS side).
 */
#define PANEL_SWAP_SIZE                 0x46c
#define PANEL_SWAP_ID                   0x50
#define PANEL_SWAP_SURFACE              0x54
#define PANEL_SWAP_SOURCE               0x64    /* x, y, width, height in the surface */
#define PANEL_SWAP_ENABLED              0xa4
#define PANEL_SWAP_DEST                 0xc4    /* x, y, width, height on the display */
#define PANEL_SWAP_LAYERS_SET           0x104   /* bit per layer given in this swap */
#define PANEL_SELECTOR_GET_DISPLAY_SIZE 8
#define PANEL_SELECTOR_SWAP_BEGIN       4
#define PANEL_SELECTOR_SWAP_END         5

static void *panelLibrary;
static io_connect_t panelFramebuffer;           /* 0: not opened yet */
static uint32_t panelSid;

/* IOSurface.framework is not in air_run's link line (Mesa's build knows IOKit and CoreFoundation),
 * and the port's own library takes IOSurfaces by another route. Here it is found by name. */
static void *
panel_symbol(const char *name)
{
   void *symbol;

   if (panelLibrary == NULL) {
      panelLibrary = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_LAZY);
      if (panelLibrary == NULL) {
         fprintf(stderr, "air_run: --panel: IOSurface.framework: %s\n", dlerror());
         return NULL;
      }
   }
   symbol = dlsym(panelLibrary, name);
   if (symbol == NULL)
      fprintf(stderr, "air_run: --panel: IOSurface.framework has no %s\n", name);
   return symbol;
}

/* A surface of the pixels, 32 bits each, blue first: the kind the framebuffer shows. */
static void *
panel_surface_create(uint32_t width, uint32_t height)
{
   typedef void *(*create_fn)(CFDictionaryRef);
   typedef uint32_t (*id_fn)(void *);
   create_fn create = (create_fn)panel_symbol("IOSurfaceCreate");
   id_fn get_id = (id_fn)panel_symbol("IOSurfaceGetID");
   CFMutableDictionaryRef properties;
   CFStringRef key;
   CFNumberRef number;
   void *surface;
   int value;

   if (create == NULL || get_id == NULL)
      return NULL;
   properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
   if (properties == NULL)
      return NULL;
#define PANEL_PROPERTY(name, number_value) \
   do { \
      key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8); \
      value = (number_value); \
      number = CFNumberCreate(NULL, kCFNumberIntType, &value); \
      CFDictionarySetValue(properties, key, number); \
      CFRelease(number); \
      CFRelease(key); \
   } while (0)
   PANEL_PROPERTY("IOSurfaceWidth", (int)width);
   PANEL_PROPERTY("IOSurfaceHeight", (int)height);
   PANEL_PROPERTY("IOSurfacePixelFormat", 0x42475241 /* 'BGRA' */);
   PANEL_PROPERTY("IOSurfaceBytesPerElement", 4);
#undef PANEL_PROPERTY
   surface = create(properties);
   CFRelease(properties);
   if (surface != NULL)
      panelSid = get_id(surface);
   return surface;
}

static bool
panel_framebuffer_open(void)
{
   io_service_t service;

   if (panelFramebuffer != 0)
      return true;
   service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOMobileFramebuffer"));
   if (service == 0) {
      fprintf(stderr, "air_run: --panel: there is no IOMobileFramebuffer service\n");
      return false;
   }
   if (IOServiceOpen(service, mach_task_self(), 0, &panelFramebuffer) != 0) {
      IOObjectRelease(service);
      fprintf(stderr, "air_run: --panel: IOMobileFramebuffer would not open\n");
      return false;
   }
   IOObjectRelease(service);
   return true;
}

static bool
panel_display_size(uint32_t *width, uint32_t *height)
{
   uint64_t size[2] = { 0, 0 };
   uint32_t count = 2;

   if (!panel_framebuffer_open())
      return false;
   if (IOConnectCallScalarMethod(panelFramebuffer, PANEL_SELECTOR_GET_DISPLAY_SIZE, NULL, 0,
                                 size, &count) != KERN_SUCCESS || count < 2)
      return false;
   *width = (uint32_t)size[0];
   *height = (uint32_t)size[1];
   return *width != 0 && *height != 0;
}

static void
panel_word(uint8_t *place, int32_t value)
{
   memcpy(place, &value, sizeof(value));
}

/* The swap the window server makes: layer 0 is this surface, the whole of it, over the whole display. */
static bool
panel_swap(uint32_t sid, uint32_t surfaceWidth, uint32_t surfaceHeight)
{
   uint8_t request[PANEL_SWAP_SIZE];
   uint8_t answer[0x40];
   size_t answerSize = sizeof(answer);
   uint64_t swap = 0;
   uint32_t count = 1;
   uint32_t width = 0, height = 0;
   int32_t source[4], destination[4];
   kern_return_t result;

   if (!panel_display_size(&width, &height))
      return false;
   result = IOConnectCallScalarMethod(panelFramebuffer, PANEL_SELECTOR_SWAP_BEGIN, NULL, 0, &swap, &count);
   if (result != KERN_SUCCESS || count < 1) {
      fprintf(stderr, "air_run: --panel: the framebuffer gave no swap number (%#x)\n", (unsigned)result);
      return false;
   }
   source[0] = 0; source[1] = 0;
   source[2] = (int32_t)surfaceWidth; source[3] = (int32_t)surfaceHeight;
   destination[0] = 0; destination[1] = 0;
   destination[2] = (int32_t)width; destination[3] = (int32_t)height;
   memset(request, 0, sizeof(request));
   panel_word(request + PANEL_SWAP_ID, (int32_t)swap);
   panel_word(request + PANEL_SWAP_SURFACE, (int32_t)sid);
   memcpy(request + PANEL_SWAP_SOURCE, source, sizeof(source));
   panel_word(request + PANEL_SWAP_ENABLED, 1);
   memcpy(request + PANEL_SWAP_DEST, destination, sizeof(destination));
   panel_word(request + PANEL_SWAP_LAYERS_SET, 1);
   result = IOConnectCallStructMethod(panelFramebuffer, PANEL_SELECTOR_SWAP_END, request, sizeof(request),
                                      answer, &answerSize);
   if (result != KERN_SUCCESS) {
      fprintf(stderr, "air_run: --panel: the swap was refused (%#x)\n", (unsigned)result);
      return false;
   }
   printf("air_run: --panel: swap %llu taken: surface %u is on its way to the panel, the framebuffer "
          "waiting for the GPU itself\n", (unsigned long long)swap, sid);
   return true;
}

#if defined(GALLIUM_FREEDRENO)
/*
 * The surface's memory as a render target: the kernel's GPU driver makes a buffer of it (the
 * surface's ID is the "file descriptor", see tools/mesa/freedreno-darwin.patch), the way
 * osmesa_surface_resource() does for the window server's layers.
 */
static struct pipe_resource *
panel_resource(struct pipe_screen *screen, uint32_t surface)
{
   struct hakios_drm_surface description = { .surface = surface };
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,
   };
   struct winsys_handle handle = { .type = WINSYS_HANDLE_TYPE_FD };

   if (hakios_drm_surface(-1, &description) != 0)
      return NULL;
   if (description.pixel_format != 0x42475241 /* 'BGRA' */ || description.planes != 0)
      return NULL;
   templ.width0 = description.width;
   templ.height0 = description.height;
   handle.handle = surface;
   handle.stride = description.row_bytes;
   handle.offset = description.offset;
   handle.size = description.size;
   return screen->resource_from_handle(screen, &templ, &handle, 0);
}
#endif
#endif /* __APPLE__ */

/* set by --library: the fill below is then floats, for the reason given there */
static bool library_fill = false;

/* The argument buffer of a wrapped library function holds floats, not the byte
 * pattern.
 *
 * Every one of these functions is compiled with no-nans-fp-math and
 * no-infs-fp-math: the module promises its inputs are finite. Arbitrary bytes
 * are mostly not -- they are infinities, NaNs and denormals -- so the inputs
 * would land outside the contract the shader was compiled under, where
 * 0 * inf may legally be either NaN or inf and a disagreement means nothing.
 * One case did disagree exactly there (folded NaN, softpipe +inf) and said
 * nothing about either side.
 *
 * Word w is ((w * 13) % 29 - 14) / 4: finite, exactly representable in
 * binary32, and different for 29 words running -- so a harness that mixed up
 * two slots, or two lanes of one slot, would get a different answer. cpuref.py
 * fills the same words.
 */
static uint8_t
library_byte(unsigned i)
{
   float f = (float)((int)((i / 4 * 13) % 29) - 14) / 4.0f;
   uint32_t u;
   memcpy(&u, &f, sizeof(u));
   return (u >> (8 * (i % 4))) & 0xff;
}

/* the pattern cpuref.py and air2nir --eval fill their buffers with, so that
 * all three agree on what the shader was given
 */
static uint8_t
pattern_byte(unsigned i)
{
   return library_fill ? library_byte(i) : (i * 7 + 13) & 0xff;
}

/* Fill a buffer the shader is about to read.
 *
 * AIR_RUN_BUFFER=FILE puts that file's bytes there instead of the pattern,
 * which is what lets a test choose the arguments an operation is called on.
 * The reason it has to be a buffer and not a constant in the shader: ir3
 * folds, so a shader that computes air.fast_round(0.5) from an immediate is
 * answered by the compiler and the Adreno never runs the instruction -- the
 * test would pass without the hardware having been asked. A value loaded from
 * a buffer cannot be folded, and the harness knows what it put there.
 *
 * Returns how many bytes came from the file, so the caller can say so rather
 * than claim a fill it did not do. A file shorter than the buffer leaves the
 * rest as the pattern.
 */
static unsigned
fill_buffer(uint8_t *bits, unsigned bytes)
{
   for (unsigned k = 0; k < bytes; k++)
      bits[k] = pattern_byte(k);

   const char *path = getenv("AIR_RUN_BUFFER");
   if (!path || !*path)
      return 0;

   FILE *f = fopen(path, "rb");
   if (!f) {
      fprintf(stderr, "air_run: AIR_RUN_BUFFER=%s: %s\n", path, strerror(errno));
      return 0;
   }
   size_t got = fread(bits, 1, bytes, f);
   fclose(f);
   return got;
}

/* A vertex shader for the quad: it takes the position as attribute 0 and
 * passes it through. Written here rather than translated, because what is
 * under test is the fragment shader; when the .metallib has a vertex shader of
 * its own that is a separate run.
 */
static nir_shader *
quad_vertex_shader(const nir_shader_compiler_options *options, unsigned num_varyings)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, options, "quad");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in,
                                          glsl_vec4_type(), "pos_in");
   in->data.location = VERT_ATTRIB_GENERIC0;
   in->data.driver_location = 0;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "pos_out");
   out->data.location = VARYING_SLOT_POS;
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);

   /* One constant coordinate per varying the fragment shader reads: the
    * centre of texel (TEX_X, TEX_Y), so every fragment samples the same
    * texel and the expected value is a single known one.
    */
   float c = (TEX_X + 0.5f) / TEX_SIZE;
   float d = (TEX_Y + 0.5f) / TEX_SIZE;
   for (unsigned i = 0; i < num_varyings; i++) {
      nir_variable *v = nir_variable_create(b.shader, nir_var_shader_out,
                                            glsl_vec4_type(), "varying");
      v->data.location = VARYING_SLOT_VAR0 + i;
      v->data.driver_location = 1 + i;
      v->data.interpolation = INTERP_MODE_SMOOTH;
      nir_store_var(&b, v, nir_imm_vec4(&b, c, d, c, 1.0f), 0xf);
   }
   b.shader->num_inputs = 1;
   b.shader->num_outputs = 1 + num_varyings;
   return b.shader;
}

/* What a Gallium driver expects of a shader it is handed: NIR's own system
 * values lowered to the hardware's, and I/O as intrinsics rather than
 * variables. The same three passes are in tools/mesa/air-ir3.patch; when a
 * fourth place needs them they should become one function of the frontend.
 */
static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/*
 * --solid: one quad, one colour, no input at all. (0, 1, 0, 1) is exact in
 * float and unmistakable beside the red clear, so the pixel says which of the
 * two writes reached the target.
 */
static nir_shader *
solid_fragment_shader(const nir_shader_compiler_options * options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, options, "solid");
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out,
                                           glsl_vec4_type(), "colour");
   out->data.location = FRAG_RESULT_DATA0;   /* what air_to_nir produces for a colour output */
   out->data.driver_location = 0;
   nir_store_var(&b, out, nir_imm_vec4(&b, 0.0f, 1.0f, 0.0f, 1.0f), 0xf);
   /* ir3 sizes its table of outputs as nir->num_outputs * 4 and indexes it by
    * the store's slot (ir3_compiler_nir.c, ctx->outputs). Left at zero, the
    * table is empty and the store writes past it -- which is what the phone
    * showed: "failed assert: idx < ctx->noutputs", and then the log stopped.
    * air_to_nir counts its own outputs the same way (air_to_nir.c, num_outputs++);
    * quad_vertex_shader above says so explicitly for the same reason.
    */
   b.shader->num_outputs = 1;
   return b.shader;
}

static void
finalize_for_gallium(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_system_values);
   /* An options struct of all-false and not NULL, which are not the same thing
    * here and the difference is a trap. nir_lower_system_values *introduces*
    * load_base_workgroup_id when it lowers SYSTEM_VALUE_WORKGROUP_ID, and the
    * only thing that removes it again is
    *
    *     if (options && !options->has_base_workgroup_id)
    *        return nir_imm_zero(...);
    *
    * so NULL reads as "the driver has base workgroup ids" -- the opposite of
    * what a harness wants -- and the intrinsic survives into the backend. With
    * softpipe that is an assertion in nir_to_tgsi rather than a message, and it
    * stopped every kernel that asks for its workgroup id. Zero is also the
    * right answer here: this dispatches one workgroup from the origin.
    */
   static const nir_lower_compute_system_values_options cs_options = { 0 };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &cs_options);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, 0);
}

/* -- compute ---------------------------------------------------------------
 *
 * A kernel writes through pointers instead of returning anything, so what is
 * read back is a device buffer. Two rules decide whether that readback is
 * evidence, and both come from the same mistake made three times with
 * textures: a test can pass for the wrong reason, and the way to stop it is
 * to make every input distinguishable so any confusion gives a wrong number
 * rather than the right one.
 *
 *   - the grid is more than one thread, and every element of the output is
 *     printed, not just the first. With one thread, or one element looked at,
 *     a wrong workgroup index or a wrong global id reads correct.
 *   - the input buffers vary with position (the same (i * 7 + 13) & 0xff
 *     pattern), so a thread reading the wrong offset produces a wrong value
 *     and not a plausible one.
 *
 * A kernel of Apple's that uses atomics and produces sensible numbers here is
 * evidence that it ran and no evidence about atomicity: a sum written by
 * threads that never raced looks exactly like a sum written by threads that
 * did. The kernel that can tell them apart is written for the purpose rather
 * than Apple's -- tools/air/atomiccheck.py, which makes each thread report the
 * value its own fetch_add returned, so the sixteen of them must be 0..15 with
 * nothing repeated whatever order the threads ran in. Even that is only
 * evidence of atomicity where the threads really do run at once, which here
 * they need not.
 */
#define GRID 4

#define MAX_IMG 16

/* The shape of each image a kernel touches, read off the intrinsics the
 * frontend emitted rather than off the Metal type name.
 *
 * air.read_texture_* and air.write_texture_* become nir_image_load and
 * nir_image_store (air_intrinsics.c), not texture instructions, so they want
 * PIPE_BIND_SHADER_IMAGE and set_shader_images and not a sampler view -- which
 * is why the draw path's texture code cannot simply be reused here.
 *
 * Per binding and not one format for the kernel: BlitLumaKernel_Unorm reads a
 * uint image and writes a half one, so a single format would be right for one
 * of its two images and quietly wrong for the other. The binding is src[0] and
 * the frontend always emits it as an immediate, so it can be read here.
 */
static void
image_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
             enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            nir_alu_type t;
            if (in->intrinsic == nir_intrinsic_image_load)
               t = nir_intrinsic_dest_type(in);
            else if (in->intrinsic == nir_intrinsic_image_store)
               t = nir_intrinsic_src_type(in);
            else
               continue;
            if (!nir_src_is_const(in->src[0]))
               continue;   /* a computed binding; refused by the caller's count */
            unsigned b = nir_src_as_uint(in->src[0]);
            if (b >= MAX_IMG || seen[b])
               continue;
            switch (nir_alu_type_get_base_type(t)) {
            case nir_type_int:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_SINT; isf[b] = false; break;
            case nir_type_uint:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_UINT; isf[b] = false; break;
            default:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_FLOAT; isf[b] = true; break;
            }
            dim[b] = nir_intrinsic_image_dim(in);
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

/* The same, for the read-only textures a kernel fetches from. A texture is an
 * AIR_RESOURCE_IMAGE only when it is written (air_to_nir.c): a read-only one
 * stays a texture and Metal's read() on it becomes a txf, a texture fetch, so
 * it needs a sampler view even inside a compute shader. BlitKernel_Unorm has
 * one of each -- a txf from its source and an image_store to its destination --
 * and binding only the image left the fetch reading nothing.
 */
static void
texture_shapes(nir_shader *cs, enum pipe_format fmt[MAX_IMG], bool isf[MAX_IMG],
               enum glsl_sampler_dim dim[MAX_IMG], bool seen[MAX_IMG], unsigned *num)
{
   nir_foreach_function_impl(impl, cs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            unsigned b = t->texture_index;
            if (b >= MAX_IMG || seen[b])
               continue;
            switch (nir_alu_type_get_base_type(t->dest_type)) {
            case nir_type_int:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_SINT; isf[b] = false; break;
            case nir_type_uint:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_UINT; isf[b] = false; break;
            default:
               fmt[b] = PIPE_FORMAT_R32G32B32A32_FLOAT; isf[b] = true; break;
            }
            dim[b] = t->sampler_dim;
            seen[b] = true;
            *num = MAX2(*num, b + 1);
         }
      }
   }
}

static int
dispatch_kernel(struct pipe_screen *screen, const struct air_shader_info *info,
                nir_shader *cs, const char *name, const char *how)
{
   /* Metal's threadgroup size is a dispatch argument, not a shader constant,
    * so the frontend leaves it variable (docs/air.md section 7.4) and whoever
    * dispatches has to fix it. air_run is that whoever. It is not optional:
    * softpipe reads the block size from TGSI_PROPERTY_CS_FIXED_BLOCK_*, and
    * with a variable size those are zero, so it runs ZERO threads and the
    * output buffer comes back exactly as it went in. That looked like a
    * kernel that did nothing, and was only visible because the buffer was
    * filled with a distinctive pattern -- had it been zeroed, a kernel named
    * InitMinMax writing zeros would have been indistinguishable from this.
    */
   cs->info.workgroup_size[0] = GRID;
   cs->info.workgroup_size[1] = GRID;
   cs->info.workgroup_size[2] = 1;
   cs->info.workgroup_size_variable = false;

   finalize_for_gallium(cs);
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   if (!pipe) {
      fprintf(stderr, "air_run: no context for a kernel\n");
      return 1;
   }
   printf("air_run: %s on %s (%s), as a compute kernel\n", name,
          screen->get_name(screen), how);

   /* every buffer the kernel declares; the device ones are what it writes */
   struct pipe_resource *bufs[32] = { 0 };
   unsigned sizes[32] = { 0 };
   bool writable[32] = { false };
   bool img_read[MAX_IMG] = { false }, img_write[MAX_IMG] = { false };
   unsigned num_img = 0;
   bool tex_used[MAX_IMG] = { false };
   unsigned num_tex = 0;
   unsigned num_ssbo = 0, num_written = 0;
   for (unsigned i = 0; i < info->num_resources; i++) {
      const struct air_resource *r = &info->resources[i];
      unsigned bytes = r->size ? r->size : 4096;
      if (r->index >= ARRAY_SIZE(bufs))
         continue;
      uint8_t *bits = calloc(1, bytes);
      unsigned from_file = fill_buffer(bits, bytes);
      if (from_file)
         printf("air_run: buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n", r->index, bytes, from_file);

      if (r->kind == AIR_RESOURCE_CONSTANT_BUFFER) {
         struct pipe_resource *b = pipe_buffer_create(screen, PIPE_BIND_CONSTANT_BUFFER,
                                                      PIPE_USAGE_DEFAULT, bytes);
         pipe_buffer_write(pipe, b, 0, bytes, bits);
         pipe->set_constant_buffer(pipe, PIPE_SHADER_COMPUTE, r->index, false,
                                   &(struct pipe_constant_buffer){
                                      .buffer = b, .buffer_size = bytes });
      } else if (r->kind == AIR_RESOURCE_DEVICE_BUFFER) {
         bufs[r->index] = pipe_buffer_create(screen, PIPE_BIND_SHADER_BUFFER,
                                             PIPE_USAGE_STAGING, bytes);
         pipe_buffer_write(pipe, bufs[r->index], 0, bytes, bits);
         sizes[r->index] = bytes;
         writable[r->index] = r->write;
         num_ssbo = MAX2(num_ssbo, r->index + 1);
         num_written += r->write ? 1 : 0;
      } else if (r->kind == AIR_RESOURCE_IMAGE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds an image at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         img_read[r->index] = r->read;
         img_write[r->index] = r->write;
         num_img = MAX2(num_img, r->index + 1);
      } else if (r->kind == AIR_RESOURCE_TEXTURE) {
         if (r->index >= MAX_IMG) {
            free(bits);
            fprintf(stderr, "air_run: %s binds a texture at %u, past the %u this harness "
                            "keeps room for\n", name, r->index, MAX_IMG);
            return 1;
         }
         tex_used[r->index] = true;
         num_tex = MAX2(num_tex, r->index + 1);
      }
      free(bits);
   }
   unsigned num_img_written = 0;
   for (unsigned i = 0; i < num_img; i++)
      num_img_written += img_write[i] ? 1 : 0;
   if (!num_written && !num_img_written) {
      fprintf(stderr, "air_run: %s writes no device buffer and no image, so a dispatch "
                      "would leave nothing to read back\n", name);
      return 1;
   }

   struct pipe_shader_buffer sb[32] = { 0 };
   unsigned mask = 0;
   for (unsigned i = 0; i < num_ssbo; i++) {
      sb[i].buffer = bufs[i];
      sb[i].buffer_size = sizes[i];
      if (writable[i])
         mask |= 1u << i;
   }
   pipe->set_shader_buffers(pipe, PIPE_SHADER_COMPUTE, 0, num_ssbo, sb, mask);

   /* The images, one resource each, with the format the kernel's own intrinsics
    * declare. A readable one is filled with the same texel pattern the draw
    * path uses -- texel (x,y,z) of slot n is (100+x+1000n, 200+y, 300+x+y+z,
    * 400) -- so that a kernel reading the wrong slot or the wrong texel cannot
    * come back with the right answer. A write-only one is left as the pattern
    * bytes so that a kernel which writes nothing is distinguishable from one
    * that writes zeros.
    */
   enum pipe_format ifmt[MAX_IMG];
   bool iisf[MAX_IMG];
   enum glsl_sampler_dim idim[MAX_IMG];
   bool iseen[MAX_IMG] = { false };
   unsigned nshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      ifmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      iisf[i] = true;
      idim[i] = GLSL_SAMPLER_DIM_2D;
   }
   image_shapes(cs, ifmt, iisf, idim, iseen, &nshape);
   struct pipe_resource *imgs[MAX_IMG] = { 0 };
   struct pipe_image_view ivs[MAX_IMG] = { 0 };
   for (unsigned i = 0; i < num_img; i++) {
      if (idim[i] != GLSL_SAMPLER_DIM_2D && idim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s uses an image of a kind this harness does not bind "
                         "(dim %u at %u)\n", name, idim[i], i);
         return 1;
      }
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource itempl = {
         .target = idim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = ifmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW,
      };
      imgs[i] = screen->resource_create(screen, &itempl);
      if (!imgs[i]) {
         fprintf(stderr, "air_run: the driver would not make image %u (%s)\n", i,
                 util_format_short_name(ifmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box box = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, imgs[i], 0, PIPE_MAP_WRITE, &box,
                            iisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      ivs[i] = (struct pipe_image_view){
         .resource = imgs[i],
         .format = ifmt[i],
         .access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                   (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .shader_access = (img_read[i] ? PIPE_IMAGE_ACCESS_READ : 0) |
                          (img_write[i] ? PIPE_IMAGE_ACCESS_WRITE : 0),
         .u.tex.level = 0, .u.tex.first_layer = 0,
         .u.tex.last_layer = depth - 1,
      };
   }
   if (num_img)
      pipe->set_shader_images(pipe, PIPE_SHADER_COMPUTE, 0, num_img, 0, ivs);

   /* and the read-only textures, as sampler views with the same texel pattern */
   enum pipe_format tfmt[MAX_IMG];
   bool tisf[MAX_IMG];
   enum glsl_sampler_dim tdim[MAX_IMG];
   bool tseen[MAX_IMG] = { false };
   unsigned ntshape = 0;
   for (unsigned i = 0; i < MAX_IMG; i++) {
      tfmt[i] = PIPE_FORMAT_R32G32B32A32_FLOAT;
      tisf[i] = true;
      tdim[i] = GLSL_SAMPLER_DIM_2D;
   }
   texture_shapes(cs, tfmt, tisf, tdim, tseen, &ntshape);
   struct pipe_resource *texes[MAX_IMG] = { 0 };
   struct pipe_sampler_view *views[MAX_IMG] = { 0 };
   void *csampler = NULL;
   for (unsigned i = 0; i < num_tex; i++) {
      if (!tex_used[i])
         continue;
      if (tdim[i] != GLSL_SAMPLER_DIM_2D && tdim[i] != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s fetches a texture of a kind this harness does not "
                         "bind (dim %u at %u)\n", name, tdim[i], i);
         return 1;
      }
      unsigned depth = tdim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      struct pipe_resource ttempl = {
         .target = tdim[i] == GLSL_SAMPLER_DIM_3D ? PIPE_TEXTURE_3D : PIPE_TEXTURE_2D,
         .format = tfmt[i],
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      texes[i] = screen->resource_create(screen, &ttempl);
      if (!texes[i]) {
         fprintf(stderr, "air_run: the driver would not make texture %u (%s)\n", i,
                 util_format_short_name(tfmt[i]));
         return 1;
      }
      float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
      for (unsigned z = 0; z < depth; z++)
         for (unsigned y = 0; y < TEX_SIZE; y++)
            for (unsigned x = 0; x < TEX_SIZE; x++) {
               unsigned k = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
               texel_value(i, x, y, z, &texels[k]);
               for (unsigned c = 0; c < 4; c++)
                  itexels[k + c] = (int32_t)texels[k + c];
            }
      struct pipe_box tbox = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      pipe->texture_subdata(pipe, texes[i], 0, PIPE_MAP_WRITE, &tbox,
                            tisf[i] ? (const void *)texels : (const void *)itexels,
                            TEX_SIZE * 4 * sizeof(float),
                            TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      struct pipe_sampler_view vtempl = { 0 };
      u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
      views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
   }
   if (num_tex) {
      csampler = pipe->create_sampler_state(pipe, &(struct pipe_sampler_state){
         .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_NEAREST,
         .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
         .unnormalized_coords = 1 });
      /* one entry per texture, not the address of one pointer. bind_sampler_states
       * reads num_tex pointers from what it is given, so passing &csampler made
       * the driver take whatever followed that single variable on the stack and
       * store it as samplers[1..]; fd6_texture_state then dereferenced those for
       * ->seqno and died at a wild address. It only showed with two or more
       * textures, which is why 22 kernels bound images here without trouble and
       * four did not. The draw path below always did this correctly.
       */
      void *csamplers[MAX_IMG] = { 0 };
      for (unsigned i = 0; i < num_tex && i < MAX_IMG; i++)
         csamplers[i] = csampler;
      pipe->bind_sampler_states(pipe, PIPE_SHADER_COMPUTE, 0, num_tex, csamplers);
      pipe->set_sampler_views(pipe, PIPE_SHADER_COMPUTE, 0, num_tex, 0, false, views);
   }

   void *cso = pipe->create_compute_state(pipe, &(struct pipe_compute_state){
      .ir_type = PIPE_SHADER_IR_NIR, .prog = cs,
      .static_shared_mem = cs->info.shared_size });
   if (!cso) {
      fprintf(stderr, "air_run: the driver would not make a compute state of it\n");
      return 1;
   }
   pipe->bind_compute_state(pipe, cso);

   /* more than one thread, and the whole grid, so that a wrong index shows */
   struct pipe_grid_info grid = {
      .work_dim = 3,
      .block = { GRID, GRID, 1 },
      .grid = { 1, 1, 1 },
   };
   printf("air_run: dispatching %ux%ux1 threads in one workgroup; %u device buffer%s bound, "
          "%u of them written, filled with (i * 7 + 13) & 0xff\n",
          GRID, GRID, num_ssbo, num_ssbo == 1 ? "" : "s", num_written);
   if (num_img || num_tex)
      printf("air_run: %u image%s bound (%u written) and %u texture%s, %ux%u, texel (x,y,z) of "
             "slot n = (100+x+1000n, 200+y, 300+x+y+z, 400)\n", num_img,
             num_img == 1 ? "" : "s", num_img_written, num_tex, num_tex == 1 ? "" : "s",
             TEX_SIZE, TEX_SIZE);
   pipe->launch_grid(pipe, &grid);
   pipe->flush(pipe, NULL, 0);

   for (unsigned i = 0; i < num_ssbo; i++) {
      if (!writable[i])
         continue;
      struct pipe_transfer *xfer;
      uint32_t *p = pipe_buffer_map(pipe, bufs[i], PIPE_MAP_READ, &xfer);
      if (!p) {
         fprintf(stderr, "air_run: device buffer %u could not be read back\n", i);
         return 1;
      }
      unsigned words = MIN2(sizes[i] / 4, (unsigned)(GRID * GRID));
      printf("air_run: device buffer %u after the dispatch, first %u words:\n", i, words);
      for (unsigned w = 0; w < words; w++)
         printf("  [%2u] %11u  0x%08x\n", w, p[w], p[w]);
      pipe_buffer_unmap(pipe, xfer);
   }

   /* and every image the kernel wrote: without this a dispatch that writes only
    * an image -- which is most of the corpus's kernels -- would have nothing to
    * read back, and air_run would have reported a successful run of something
    * it had not looked at.
    */
   for (unsigned i = 0; i < num_img; i++) {
      if (!img_write[i])
         continue;
      struct pipe_transfer *xfer;
      unsigned depth = idim[i] == GLSL_SAMPLER_DIM_3D ? TEX_SIZE : 1;
      void *pix = pipe->texture_map(pipe, imgs[i], 0, PIPE_MAP_READ,
                                    &(struct pipe_box){ .width = TEX_SIZE,
                                                        .height = TEX_SIZE,
                                                        .depth = depth }, &xfer);
      if (!pix) {
         fprintf(stderr, "air_run: image %u could not be read back\n", i);
         return 1;
      }
      printf("air_run: image %u after the dispatch, texels (0,0) and (1,1):\n", i);
      for (unsigned t = 0; t < 2; t++) {
         unsigned x = t, y = t;
         for (unsigned c = 0; c < 4; c++) {
            unsigned k = (y * (xfer->stride / (4 * sizeof(float))) + x) * 4 + c;
            if (iisf[i])
               printf("  (%u,%u)[%u] %.9g\n", x, y, c, (double)((float *)pix)[k]);
            else
               printf("  (%u,%u)[%u] %11d\n", x, y, c, ((int32_t *)pix)[k]);
         }
      }
      pipe->texture_unmap(pipe, xfer);
   }

   pipe->destroy(pipe);
   screen->destroy(screen);
   return 0;
}

int
main(int argc, char **argv)
{
   /* --disasm: ask ir3 to print the Adreno instructions it generates. The
    * screen's name proves fd_screen_create won; it does not prove that the
    * shader itself went through ir3 rather than some later software path, and
    * the four floats look identical either way. ir3's own disassembly in the
    * log is the positive proof. IR3_SHADER_DEBUG is read once, at compiler
    * creation, so it has to be set before the screen is made.
    */
   bool want_disasm = false;
   /* --library runs an air.visible library function through the generated
    * wrapper (nir_air.h): there is nothing to dispatch otherwise, so these 135
    * modules were never run at all.
    */
   bool want_library = false;
   /* --solid skips AIR entirely and draws one literal colour: a pixel that is
    * not that colour then says the draw never reached the render target, not
    * that the translated shader computed something else. It is the harness's
    * way of telling the pipeline and the compiler apart.
    */
   bool want_solid = false;
   /* --panel draws into an IOSurface and hands it to the framebuffer's swap, so the picture is on
    * the panel and not only in a number this program prints.
    */
   bool want_panel = false;
   /* --frames N repeats the submission, the fence wait and the swap N times: a path that
    * lands once and then stalls is not a working path, and a swap taken once and then
    * wedged is not either.
    */
   unsigned want_frames = 1;
   /* --buffer FILE is AIR_RUN_BUFFER as an argument, and it exists because the
    * device has no shell to set one with. iOS 16.5's own root carries no sh at
    * all -- /bin holds df and ps -- and the job list that runs this is read by
    * our init, which splits a line on blanks and has no notion of a variable
    * (userland/src/init.c, run_command). So a generated suite whose every
    * module reads its own bytes could not be run on the phone until this
    * option existed: with it each module is one ordinary job line.
    */
   while (argc > 1 && argv[1][0] == '-') {
      if (!strcmp(argv[1], "--disasm")) {
         want_disasm = true;
         setenv("IR3_SHADER_DEBUG", "disasm", 1);
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--library")) {
         want_library = library_fill = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--solid")) {
         want_solid = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--panel")) {
         want_panel = true;
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--sysmem")) {
         /* Render straight into system memory: with tiling on, the draw lands
          * in the GMEM buffer and a resolve pass copies it out, and a missing
          * or failing resolve is one more way for a correct draw to read back
          * as nothing. freedreno's own name for this is FD_MESA_DEBUG=sysmem.
          */
         setenv("FD_MESA_DEBUG", "sysmem", 1);
         argv++;
         argc--;
      } else if (!strcmp(argv[1], "--frames") && argc > 2) {
         want_frames = strtoul(argv[2], NULL, 0);
         if (want_frames == 0)
            want_frames = 1;
         argv += 2;
         argc -= 2;
      } else if (!strcmp(argv[1], "--buffer") && argc > 2) {
         setenv("AIR_RUN_BUFFER", argv[2], 1);
         argv += 2;
         argc -= 2;
      } else {
         break;
      }
   }
   if (argc < 2) {
      fprintf(stderr, "usage: air_run [--disasm] [--library] [--buffer FILE] [--solid] [--panel] [--frames N] "
                      "[--sysmem] "
                      "FILE[.metallib] [FUNCTION]\n");
      return 2;
   }

   watch_for_faults();
   glsl_type_singleton_init_or_ref();
   void *ctx = ralloc_context(NULL);

   /* the module */
   FILE *f = fopen(argv[1], "rb");
   if (!f) {
      perror(argv[1]);
      return 1;
   }
   fseek(f, 0, SEEK_END);
   size_t size = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *data = ralloc_size(ctx, size);
   if (fread(data, 1, size, f) != size) {
      fprintf(stderr, "air_run: short read of %s\n", argv[1]);
      return 1;
   }
   fclose(f);

   const void *bitcode = data;
   size_t bitcode_size = size;
   const char *want = argc > 2 ? argv[2] : NULL;
   const char *name = argv[1];
   if (size >= 4 && !memcmp(data, "MTLB", 4)) {
      struct air_metallib lib;
      const char *err;
      if (!air_metallib_parse(ctx, data, size, &lib, &err)) {
         fprintf(stderr, "air_run: %s: %s\n", argv[1], err);
         return 1;
      }
      unsigned pick = lib.num_functions;
      for (unsigned i = 0; i < lib.num_functions; i++) {
         if (!want || !strcmp(want, lib.functions[i].name)) {
            pick = i;
            break;
         }
      }
      if (pick == lib.num_functions) {
         fprintf(stderr, "air_run: no function %s\n", want ? want : "(any)");
         return 1;
      }
      bitcode = lib.functions[pick].bitcode;
      bitcode_size = lib.functions[pick].bitcode_size;
      name = lib.functions[pick].name;
   }

   /* The screen: the GPU's where the kernel has one to drive, softpipe
    * otherwise. The same source therefore runs on this machine (no msm
    * device, so it falls back and draws in software) and on the phone, where
    * it opens the Adreno exactly as the port's own Mesa does
    * (osmesa_create_gpu_screen in tools/mesa/freedreno-darwin.patch).
    * AIR_RUN_SOFTWARE=1 forces software even where a GPU is present.
    */
   struct pipe_screen *screen = NULL;
   const char *how = "software";
#ifdef GALLIUM_FREEDRENO
   if (!debug_get_bool_option("AIR_RUN_SOFTWARE", false)) {
      int fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
      if (fd >= 0) {
         /* The driver option cache has to be real, not a zeroed struct:
          * fd_screen_create passes it straight to driParseConfigFiles, which
          * dereferences it. A zeroed one died at address 0x10 on the phone --
          * the first thing this program ever did on a machine that has a GPU,
          * and invisible on any machine that has not. This is the sequence
          * osmesa_create_gpu_screen() uses in tools/mesa/freedreno-darwin.patch,
          * which is the one CoreAnimation goes through on this device daily.
          */
         static const driOptionDescription freedreno_options[] = {
#include "freedreno/driinfo_freedreno.h"
         };
         static driOptionCache info, cache;
         static struct pipe_screen_config config;

         driParseOptionInfo(&info, freedreno_options, ARRAY_SIZE(freedreno_options));
         driParseConfigFiles(&cache, &info, 0, "msm", NULL, NULL, NULL, 0, NULL, 0);
         config.options = &cache;
         config.options_info = &info;

         screen = fd_drm_screen_create_renderonly(fd, NULL, &config);
         if (screen)
            how = "the GPU, through the kernel's msm device";
         else
            fprintf(stderr, "air_run: the msm device opened but gave no screen\n");
      }
   }
#endif
   if (!screen)
      screen = sw_screen_create(null_sw_create());
   if (!screen) {
      fprintf(stderr, "air_run: no gallium screen\n");
      return 1;
   }
   /* Named before the line is printed and not after it: with --solid the
    * metallib's function is picked and then never translated, and a run that
    * still announced clearVertex left the log unable to say which path ran.
    */
   if (want_solid)
      name = "solid";
   printf("air_run: %s on %s (%s)\n", name, screen->get_name(screen), how);

#if defined(__APPLE__) && defined(GALLIUM_FREEDRENO)
   struct pipe_resource *panel = NULL;

   if (want_panel) {
      uint32_t width = 0, height = 0;
      void *surface;

      if (!panel_display_size(&width, &height)) {
         fprintf(stderr, "air_run: --panel: no display to ask the size of\n");
         return 1;
      }
      surface = panel_surface_create(width, height);
      if (surface == NULL) {
         fprintf(stderr, "air_run: --panel: no IOSurface of %ux%u\n", width, height);
         return 1;
      }
      panel = panel_resource(screen, panelSid);
      CFRelease(surface);
      if (panel == NULL) {
         fprintf(stderr, "air_run: --panel: surface %u is not a buffer of this screen\n", panelSid);
         return 1;
      }
      printf("air_run: --panel: IOSurface %u, %ux%u, is the render target (a buffer of the GPU, "
             "B8G8R8A8)\n", panelSid, width, height);
   }
#else
   if (want_panel) {
      fprintf(stderr, "air_run: --panel: this air_run has no framebuffer to talk to\n");
      return 1;
   }
#endif

   const void *options =
      screen->get_compiler_options(screen, PIPE_SHADER_IR_NIR, PIPE_SHADER_FRAGMENT);
   if (!options) {
      fprintf(stderr, "air_run: this screen takes no NIR\n");
      return 1;
   }

   /* the shader */
   struct air_constant_value *fc;
   unsigned num_fc = air_module_function_constants(ctx, bitcode, bitcode_size, &fc);
   struct air_to_nir_options air_options = { .constants = fc, .num_constants = num_fc,
                                            .library_wrapper = want_library };
   struct air_shader_info info;
   const char *err = NULL;
   nir_shader *fs;
   if (want_solid) {
      bzero(&info, sizeof(info));
      info.stage = MESA_SHADER_FRAGMENT;
      fs = solid_fragment_shader(options);
      name = "solid";
   } else {
      fs = air_to_nir(ctx, bitcode, bitcode_size, NULL, &air_options, options,
                      &info, &err);
      if (!fs) {
         fprintf(stderr, "air_run: %s: %s\n", name, err);
         return 1;
      }
   }
   if (info.stage == MESA_SHADER_COMPUTE)
      return dispatch_kernel(screen, &info, fs, name, how);
   if (info.stage != MESA_SHADER_FRAGMENT) {
      fprintf(stderr, "air_run: %s is a %s shader; only fragment shaders are drawn here\n",
              name, gl_shader_stage_name(info.stage));
      return 1;
   }
   /* The render target's format follows the shader's own output type. A Metal
    * fragment shader returning uint4 writes integers, and reading those back
    * through a float target gives the right bits read the wrong way -- which
    * looks exactly like a translation bug and is not one.
    */
   enum pipe_format rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   bool rt_is_float = true;
   bool rt_is_half = false;
   /* Which attachment to read back: the lowest one the shader writes, not
    * target 0. clearFragment04 writes FRAG_RESULT_DATA1 alone, and target 0
    * then holds the clear colour -- a correct result that looks exactly like
    * a shader which stored nothing.
    */
   unsigned read_rt = ~0u;
   nir_foreach_variable_with_modes(var, fs, nir_var_shader_out) {
      if (var->data.location < FRAG_RESULT_DATA0)
         continue;
      unsigned idx = var->data.location - FRAG_RESULT_DATA0;
      if (idx >= read_rt)
         continue;
      read_rt = idx;
      enum glsl_base_type base = glsl_get_base_type(glsl_without_array(var->type));
      if (base != GLSL_TYPE_FLOAT && base != GLSL_TYPE_FLOAT16 &&
          base != GLSL_TYPE_DOUBLE) {
         /* unsigned, not signed: an LLVM integer carries no signedness, so
          * both this and tools/air/cpuref.py print the bits unsigned and the
          * two can be compared
          */
         rt_format = PIPE_FORMAT_R32G32B32A32_UINT;
         rt_is_float = false;
      } else if (base == GLSL_TYPE_FLOAT16) {
         /* A half return needs a half target. Through a 32-bit one the half is
          * widened the moment it is written, and NIR then folds the conversion
          * that produced it into a 32-bit one: the generated Adreno code for
          * air.convert.f.f16.u.i32 was four cov.u32f32 and not one cov.u32f16,
          * so the shader computed f32(x) while the reference computed f16(x)
          * and the comparison meant nothing. Ten declarations of the corpus
          * return half and every one of them was being reported as agreeing on
          * the strength of a case that was never compared (docs/air.md,
          * section 4 item 4).
          *
          * The chain is still u32 -> f32 -> f16 rather than a single
          * conversion wherever NIR widens before the store, so a value needing
          * to round at both steps can differ from one direct conversion by
          * double rounding. None of the pool's values does, but a future one
          * might, and that is a property of the harness and not of the GPU.
          */
         /* As of now nothing reaches this branch: tools/air/intrincheck.py packs
          * every float-ish result into a float4, so no generated shader declares
          * a half output. It is here because the target was one of the two things
          * missing and is the half that belongs on this side; the other half needs
          * a half render target in the generator, whose air.arg_type_name the
          * corpus does not attest (docs/air.md, section 4 item 4).
          */
         rt_format = PIPE_FORMAT_R16G16B16A16_FLOAT;
         rt_is_float = true;
         rt_is_half = true;
      } else {
         rt_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
         rt_is_float = true;
      }
   }
   if (read_rt == ~0u) {
      fprintf(stderr, "air_run: %s writes no colour attachment\n", name);
      return 1;
   }

   /* A shader may declare a render target and never store to it:
    * texturedFragment3DDepth returns {undef, depth} and its only real output
    * is FRAG_RESULT_DEPTH. Reading the colour then gives the clear value,
    * which is a number that means nothing -- worse than no number, because it
    * looks like a result. This harness binds no depth attachment, so say so.
    */
   bool stores_colour = false;
   nir_foreach_function_impl(impl, fs) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
            if (d && d->deref_type == nir_deref_type_var &&
                d->var->data.location == (int)(FRAG_RESULT_DATA0 + read_rt))
               stores_colour = true;
         }
      }
   }
   if (!stores_colour) {
      fprintf(stderr, "air_run: %s stores nothing to a colour attachment -- its output is "
                      "depth or stencil, which this harness does not read back\n", name);
      return 1;
   }

   finalize_for_gallium(fs);

   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);

   /* The port's memory, the processor's side alone: a texture written and
    * read back through the map, with no GPU involved. Nothing had exercised
    * this yet, and a map that does not round-trip makes every GPU answer read
    * as zero -- the shape of result the first run on the phone gave.
    */
   if (pipe) {
      struct pipe_box whole = { .width = WIDTH, .height = HEIGHT, .depth = 1 };
      struct pipe_resource *probe = screen->resource_create(screen,
         &(struct pipe_resource){ .target = PIPE_TEXTURE_2D,
            .format = PIPE_FORMAT_R32G32B32A32_FLOAT, .width0 = WIDTH, .height0 = HEIGHT,
            .depth0 = 1, .array_size = 1, .bind = PIPE_BIND_SAMPLER_VIEW });
      struct pipe_transfer *xfer = NULL;
      uint32_t *words = probe ?
         pipe->texture_map(pipe, probe, 0, PIPE_MAP_WRITE, &whole, &xfer) : NULL;
      if (!words) {
         printf("air_run: the port's memory: a texture could not be mapped\n");
      } else {
         for (unsigned i = 0; i < WIDTH * HEIGHT * 4; i++)
            words[i] = 0x12345678;
         pipe->texture_unmap(pipe, xfer);
         uint32_t *again = pipe->texture_map(pipe, probe, 0, PIPE_MAP_READ, &whole, &xfer);
         unsigned wrong = ~0u;
         if (again) {
            wrong = 0;
            for (unsigned i = 0; i < WIDTH * HEIGHT * 4; i++)
               wrong += again[i] != 0x12345678;
            pipe->texture_unmap(pipe, xfer);
         }
         printf("air_run: the port's memory: a texture the processor wrote reads back %s\n",
                wrong == 0 ? "the same" :
                wrong == ~0u ? "nothing (the second map failed)" : "different words");
      }
   }

   /* One target per render target the shader writes: a shader whose outputs
    * outnumber the framebuffer's attachments draws nothing at all, which
    * looks like a translation that produced no stores.
    */
   unsigned num_rt = MAX2(info.num_render_targets, read_rt + 1);
   if (num_rt > PIPE_MAX_COLOR_BUFS) {
      fprintf(stderr, "air_run: %u render targets, more than this can bind\n", num_rt);
      return 1;
   }
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = rt_format,
      .width0 = WIDTH,
      .height0 = HEIGHT,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *target = NULL;
#if defined(__APPLE__) && defined(GALLIUM_FREEDRENO)
   if (panel != NULL) {
      /* the surface is BGRA and of its own size: the shader's output goes into it as it is */
      templ.format = panel->format;
      templ.width0 = panel->width0;
      templ.height0 = panel->height0;
   }
#endif
   struct pipe_framebuffer_state fb = {
      .width = templ.width0, .height = templ.height0, .nr_cbufs = num_rt,
   };
   for (unsigned i = 0; i < num_rt; i++) {
      struct pipe_resource *res = screen->resource_create(screen, &templ);
#if defined(__APPLE__) && defined(GALLIUM_FREEDRENO)
      if (panel != NULL && i == read_rt)
         res = panel;                   /* (the surface's, not ours to free) */
#endif
      fb.cbufs[i] = pipe->create_surface(pipe, res,
         &(struct pipe_surface){ .format = templ.format });
      if (i == read_rt)
         target = res;
   }
   pipe->set_framebuffer_state(pipe, &fb);

   pipe->set_viewport_states(pipe, 0, 1, &(struct pipe_viewport_state){
      .scale = { fb.width / 2.0f, fb.height / 2.0f, 1.0f },
      .translate = { fb.width / 2.0f, fb.height / 2.0f, 0.0f },
   });

   void *blend = pipe->create_blend_state(pipe, &(struct pipe_blend_state){
      .rt[0].colormask = PIPE_MASK_RGBA,
   });
   pipe->bind_blend_state(pipe, blend);
   void *dsa = pipe->create_depth_stencil_alpha_state(pipe,
      &(struct pipe_depth_stencil_alpha_state){ 0 });
   pipe->bind_depth_stencil_alpha_state(pipe, dsa);
   void *rast = pipe->create_rasterizer_state(pipe, &(struct pipe_rasterizer_state){
      .cull_face = PIPE_FACE_NONE, .half_pixel_center = 1, .bottom_edge_rule = 1,
      .depth_clip_near = 1, .depth_clip_far = 1,
   });
   pipe->bind_rasterizer_state(pipe, rast);

   /* every constant buffer the shader reads, filled with the pattern */
   for (unsigned i = 0; i < info.num_resources; i++) {
      const struct air_resource *r = &info.resources[i];
      if (r->kind != AIR_RESOURCE_CONSTANT_BUFFER && r->kind != AIR_RESOURCE_DEVICE_BUFFER)
         continue;
      unsigned bytes = r->size ? r->size : 256;
      uint8_t *bits = malloc(bytes);
      unsigned from_file = fill_buffer(bits, bytes);
      struct pipe_resource *buf = pipe_buffer_create(screen, PIPE_BIND_CONSTANT_BUFFER,
                                                     PIPE_USAGE_DEFAULT, bytes);
      pipe_buffer_write(pipe, buf, 0, bytes, bits);
      pipe->set_constant_buffer(pipe, PIPE_SHADER_FRAGMENT, r->index, false,
                                &(struct pipe_constant_buffer){
                                   .buffer = buf, .buffer_size = bytes,
                                });
      if (from_file)
         printf("air_run: %s buffer at index %u, %u bytes, %u of them from "
                "AIR_RUN_BUFFER\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes, from_file);
      else
         printf("air_run: %s buffer at index %u, %u bytes of (i * 7 + 13) & 0xff\n",
                r->kind == AIR_RESOURCE_CONSTANT_BUFFER ? "constant" : "device",
                r->index, bytes);
      free(bits);
   }

   /* every sampled texture the shader declares, and a sampler for it */
   unsigned num_tex = 0;
   for (unsigned i = 0; i < info.num_resources; i++)
      if (info.resources[i].kind == AIR_RESOURCE_TEXTURE)
         num_tex = MAX2(num_tex, info.resources[i].index + 1);
   if (num_tex) {
      /* The texture's format follows what the shader samples into: a Metal
       * texture2d<uint> read out of a float texture gives numbers that are
       * neither right nor obviously wrong.
       */
      enum pipe_format tex_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
      bool tex_is_float = true;
      /* and the target, which has to match too: a 3D sample from a 2D
       * texture can come back right by accident, which is not evidence
       */
      enum pipe_texture_target target = PIPE_TEXTURE_2D;
      enum glsl_sampler_dim dim = GLSL_SAMPLER_DIM_2D;
      nir_foreach_function_impl(impl, fs) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;
               nir_tex_instr *t = nir_instr_as_tex(instr);
               nir_alu_type base = nir_alu_type_get_base_type(t->dest_type);
               if (base == nir_type_int) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_SINT;
                  tex_is_float = false;
               } else if (base == nir_type_uint) {
                  tex_format = PIPE_FORMAT_R32G32B32A32_UINT;
                  tex_is_float = false;
               }
               dim = t->sampler_dim;
               if (dim == GLSL_SAMPLER_DIM_3D)
                  target = PIPE_TEXTURE_3D;
               else if (dim == GLSL_SAMPLER_DIM_MS)
                  target = PIPE_TEXTURE_2D;   /* refused below */
               goto have_format;
            }
         }
      }
   have_format:;
      if (dim == GLSL_SAMPLER_DIM_MS) {
         fprintf(stderr, "air_run: %s samples a multisample texture; this harness binds "
                         "none, so it is not run rather than run against the wrong thing\n",
                 name);
         return 1;
      }
      if (dim != GLSL_SAMPLER_DIM_2D && dim != GLSL_SAMPLER_DIM_3D) {
         fprintf(stderr, "air_run: %s samples a texture of a kind this harness does not "
                         "bind (dim %u)\n", name, dim);
         return 1;
      }
      unsigned depth = target == PIPE_TEXTURE_3D ? TEX_SIZE : 1;
      unsigned tex_z = target == PIPE_TEXTURE_3D ? TEX_Y : 0;

      struct pipe_resource ttempl = {
         .target = target,
         .format = tex_format,
         .width0 = TEX_SIZE, .height0 = TEX_SIZE, .depth0 = depth, .array_size = 1,
         .bind = PIPE_BIND_SAMPLER_VIEW,
      };
      struct pipe_box box = { .width = TEX_SIZE, .height = TEX_SIZE, .depth = depth };
      struct pipe_resource *texes[PIPE_MAX_SAMPLERS] = { 0 };
      for (unsigned slot = 0; slot < num_tex && slot < PIPE_MAX_SAMPLERS; slot++) {
         float texels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         int32_t itexels[TEX_SIZE * TEX_SIZE * TEX_SIZE * 4];
         for (unsigned z = 0; z < depth; z++) {
            for (unsigned y = 0; y < TEX_SIZE; y++) {
               for (unsigned x = 0; x < TEX_SIZE; x++) {
                  unsigned i = ((z * TEX_SIZE + y) * TEX_SIZE + x) * 4;
                  texel_value(slot, x, y, z, &texels[i]);
                  for (unsigned c = 0; c < 4; c++)
                     itexels[i + c] = (int32_t)texels[i + c];
               }
            }
         }
         texes[slot] = screen->resource_create(screen, &ttempl);
         pipe->texture_subdata(pipe, texes[slot], 0, PIPE_MAP_WRITE, &box,
                               tex_is_float ? (const void *)texels : (const void *)itexels,
                               TEX_SIZE * 4 * sizeof(float),
                               TEX_SIZE * TEX_SIZE * 4 * sizeof(float));
      }

      void *sampler = pipe->create_sampler_state(pipe, &(struct pipe_sampler_state){
         .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_NEAREST,
         .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
         .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
      });

      struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS] = { 0 };
      void *samplers[PIPE_MAX_SAMPLERS] = { 0 };
      for (unsigned i = 0; i < num_tex && i < PIPE_MAX_SAMPLERS; i++) {
         struct pipe_sampler_view vtempl = { 0 };
         u_sampler_view_default_template(&vtempl, texes[i], texes[i]->format);
         views[i] = pipe->create_sampler_view(pipe, texes[i], &vtempl);
         samplers[i] = sampler;
      }
      pipe->set_sampler_views(pipe, PIPE_SHADER_FRAGMENT, 0, num_tex, 0, false, views);
      pipe->bind_sampler_states(pipe, PIPE_SHADER_FRAGMENT, 0, num_tex, samplers);

      float want[4];
      texel_value(0, TEX_X, TEX_Y, tex_z, want);
      printf("air_run: %u texture%s bound: %ux%ux%u %s, texel (x,y,z) of slot n = "
             "(100+x+1000n, 200+y, 300+x+y+z, 400), nearest, clamped\n",
             num_tex, num_tex == 1 ? "" : "s", TEX_SIZE, TEX_SIZE, depth,
             util_format_short_name(tex_format));
      printf("air_run: expected %.9g %.9g %.9g %.9g  (the centre of texel %u,%u,%u sampled "
             "once)\n", (double)want[0], (double)want[1], (double)want[2], (double)want[3],
             TEX_X, TEX_Y, tex_z);
   }

   /* the quad */
   static const float verts[] = {
      -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1,
   };
   struct pipe_resource *vbuf = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER,
                                                   PIPE_USAGE_DEFAULT, sizeof(verts));
   pipe_buffer_write(pipe, vbuf, 0, sizeof(verts), verts);
   pipe->set_vertex_buffers(pipe, 1, &(struct pipe_vertex_buffer){
      .buffer.resource = vbuf, .buffer_offset = 0,
   });
   void *velem = pipe->create_vertex_elements_state(pipe, 1,
      &(struct pipe_vertex_element){
         .src_offset = 0, .src_stride = 16, .vertex_buffer_index = 0,
         .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT,
      });
   pipe->bind_vertex_elements_state(pipe, velem);

   nir_shader *vs = quad_vertex_shader(options, info.num_varyings);
   finalize_for_gallium(vs);
   void *vs_cso = pipe->create_vs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = vs,
   });
   pipe->bind_vs_state(pipe, vs_cso);
   void *fs_cso = pipe->create_fs_state(pipe, &(struct pipe_shader_state){
      .type = PIPE_SHADER_IR_NIR, .ir.nir = fs,
   });
   pipe->bind_fs_state(pipe, fs_cso);

   /* --solid paints one quad green over a red clear. Green is the whole way,
    * red is the clear alone (so the draw was dropped), zero is neither -- and
    * the round-trip above then says whether the map is at fault.
    */
   union pipe_color_union clear = { .ui = { 0, 0, 0, 0 } };
   if (want_solid)
      clear = (union pipe_color_union){ .f = { 1.0f, 0.0f, 0.0f, 1.0f } };

   struct pipe_draw_info draw = {
      .mode = MESA_PRIM_TRIANGLE_STRIP, .index_bounds_valid = true,
      .max_index = 3, .instance_count = 1,
   };
   struct pipe_draw_start_count_bias sc = { .start = 0, .count = 4 };

   unsigned frames = want_frames ? want_frames : 1;

   for (unsigned frame = 0; frame < frames; frame++) {
      pipe->clear(pipe, PIPE_CLEAR_COLOR, NULL, &clear, 0.0, 0);
      pipe->draw_vbo(pipe, &draw, 0, NULL, &sc, 1);

      /* Wait on the draw's fence before the pixel is read. The map below asks
       * the kernel whether the GPU is done with the buffer (CPU_PREP), and that
       * question is answered at once when the fence has not landed yet -- the
       * first run on the phone came back with zeros for that reason. Two seconds
       * is a stop, not a hang: if the fence never lands, the run still ends and
       * says so.
       */
      struct pipe_fence_handle *fence = NULL;
      pipe->flush(pipe, &fence, 0);
      if (fence) {
         bool landed = screen->fence_finish(screen, NULL, fence, 2000000000ull);

         if (frames > 1)
            printf("air_run: frame %u/%u: the draw's fence %s\n", frame + 1, frames,
                   landed ? "landed" : "did not land within 2 s");
         else
            printf("air_run: the draw's fence %s\n", landed ? "landed" : "did not land within 2 s");
         screen->fence_reference(screen, &fence, NULL);
      }

#if defined(__APPLE__) && defined(GALLIUM_FREEDRENO)
      if (panel != NULL) {
         /* The draw's fence landed above: the kernel's GPU driver has the surface's last submission
          * recorded against it, so the framebuffer's wait is satisfied and the picture goes to the
          * panel through the same show() the window server's frames go through.
          */
         if (panel_swap(panelSid, panel->width0, panel->height0)) {
            if (frames > 1)
               printf("air_run: --panel: frame %u/%u: THE PICTURE THE GPU DREW IS ON THE PANEL\n",
                      frame + 1, frames);
            else
               printf("air_run: --panel: THE PICTURE THE GPU DREW IS ON THE PANEL\n");
         }
      }
#endif
   }

   /* the pixel */
   struct pipe_transfer *xfer;
   void *pix = pipe->texture_map(pipe, target, 0, PIPE_MAP_READ,
                                 &(struct pipe_box){ .width = WIDTH, .height = HEIGHT,
                                                     .depth = 1 }, &xfer);
   if (!pix) {
      fprintf(stderr, "air_run: the render target could not be read back\n");
      return 1;
   }
   /* Named again here, beside the numbers: the line at the top says which
    * screen was created, this one says which screen the pixel just came out
    * of. Neither is proof that no stage in between went software -- --disasm
    * is, by making ir3 print what it compiled.
    */
   printf("air_run: read back from %s (%s)%s\n", screen->get_name(screen), how,
          want_disasm ? ", ir3's instructions are above" : "");
   printf("air_run: the pixel at (0, 0) of render target %u came back as:\n",
          read_rt);
   for (unsigned c = 0; c < 4; c++) {
      if (rt_is_half)
         printf("  %.9g\n", (double)_mesa_half_to_float(((uint16_t *)pix)[c]));
      else if (rt_is_float)
         printf("  %.9g\n", (double)((float *)pix)[c]);
      else
         printf("  %u\n", ((uint32_t *)pix)[c]);
   }
   {
      /* Sixteen words rather than the four floats above, so that a zero is a
       * statement about the memory and not about one pixel: a red clear is
       * 3f800000 00000000 00000000 3f800000, a green quad 00000000 3f800000
       * 00000000 3f800000, and sixteen zeros say the core never wrote the page.
       */
      const uint32_t *words = pix;
      unsigned zeros = 0;

      printf("air_run: the target's first sixteen words:");
      for (unsigned i = 0; i < 16; i++) {
         printf(" %08x", words[i]);
         zeros += words[i] == 0;
      }
      printf("  (%u of them are zero)\n", zeros);
   }
   pipe->texture_unmap(pipe, xfer);

   printf("air_run: drew %ux%u into %s, %u render target%s\n",
          fb.width, fb.height, util_format_short_name(rt_format), num_rt,
          num_rt == 1 ? "" : "s");

   /* The target's memory, the processor's side. The probe texture above proves
    * that a map round-trips; it does not prove that this map reaches the pages
    * the kernel handed the GPU, and a map of some other allocation would read
    * as zeros for every pixel the core wrote.
    */
   bool probe_target = target != NULL;
#if defined(__APPLE__) && defined(GALLIUM_FREEDRENO)
   probe_target = probe_target && panel == NULL;        /* (the panel would show that word) */
#endif
   if (probe_target) {
      struct pipe_box whole = { .width = fb.width, .height = fb.height, .depth = 1 };
      uint32_t *over = pipe->texture_map(pipe, target, 0, PIPE_MAP_WRITE, &whole, &xfer);

      if (over) {
         uint32_t *back;

         over[0] = 0x12345678;
         pipe->texture_unmap(pipe, xfer);
         back = pipe->texture_map(pipe, target, 0, PIPE_MAP_READ, &whole, &xfer);
         printf("air_run: the processor wrote 0x12345678 over the target and read back %s\n",
                !back ? "nothing" : back[0] == 0x12345678 ? "the same" : "something else");
         if (back)
            pipe->texture_unmap(pipe, xfer);
      }
   }

   pipe->destroy(pipe);
   screen->destroy(screen);
   ralloc_free(ctx);
   glsl_type_singleton_decref();
   return 0;
}
