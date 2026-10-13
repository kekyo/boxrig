/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "runtime.h"

#include <GL/glcorearb.h>

#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef GLenum(APIENTRYP boxrig_gl_reset_status_proc)(void);

typedef struct boxrig_gl_api {
    PFNGLGETSTRINGPROC GetString;
    PFNGLGETSTRINGIPROC GetStringi;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETBOOLEANVPROC GetBooleanv;
    PFNGLGETFLOATVPROC GetFloatv;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLVIEWPORTPROC Viewport;
    PFNGLSCISSORPROC Scissor;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLBLENDEQUATIONSEPARATEPROC BlendEquationSeparate;
    PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate;
    PFNGLBLENDCOLORPROC BlendColor;
    PFNGLDEPTHMASKPROC DepthMask;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLBINDSAMPLERPROC BindSampler;
    PFNGLPIXELSTOREIPROC PixelStorei;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC
        GetFramebufferAttachmentParameteriv;
    PFNGLGETERRORPROC GetError;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLBUFFERSUBDATAPROC BufferSubData;
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLTEXIMAGE2DPROC TexImage2D;
    PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLGENSAMPLERSPROC GenSamplers;
    PFNGLDELETESAMPLERSPROC DeleteSamplers;
    PFNGLSAMPLERPARAMETERIPROC SamplerParameteri;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM4FVPROC Uniform4fv;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor;
    PFNGLDRAWARRAYSINSTANCEDPROC DrawArraysInstanced;
    PFNGLFENCESYNCPROC FenceSync;
    PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
    PFNGLDELETESYNCPROC DeleteSync;
    boxrig_gl_reset_status_proc GetGraphicsResetStatus;
} boxrig_gl_api;

typedef struct boxrig_gl_saved_state {
    GLint draw_framebuffer;
    GLint read_framebuffer;
    GLint viewport[4];
    GLint scissor_box[4];
    GLboolean scissor_enabled;
    GLboolean color_mask[4];
    GLboolean blend_enabled;
    GLint blend_equation_rgb;
    GLint blend_equation_alpha;
    GLint blend_source_rgb;
    GLint blend_source_alpha;
    GLint blend_destination_rgb;
    GLint blend_destination_alpha;
    GLfloat blend_color[4];
    GLboolean depth_enabled;
    GLboolean depth_mask;
    GLboolean stencil_enabled;
    GLboolean cull_enabled;
    GLboolean multisample_enabled;
    GLboolean framebuffer_srgb_enabled;
    GLint program;
    GLint vertex_array;
    GLint array_buffer;
    GLint pixel_unpack_buffer;
    GLint active_texture;
    GLint texture_2d;
    GLint sampler;
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint unpack_skip_rows;
    GLint unpack_skip_pixels;
} boxrig_gl_saved_state;

struct boxrig_gl_runtime {
    boxrig_gl_runtime_config config;
    boxrig_gl_current_context bound_context;
    boxrig_gl_api gl;
    boxrig_gl_capabilities capabilities;
    boxrig_gl_state_counters counters;
    boxrig_gl_saved_state saved_state;
    GLuint program;
    GLuint vertex_array;
    GLuint buffers[2];
    GLuint texture;
    GLuint sampler;
    uint64_t target_generation;
    bool target_active;
    bool srgb_write_control;
};

static void set_diagnostic(
    boxrig_gl_diagnostic *diagnostic,
    boxrig_gl_status status,
    uint32_t gl_code,
    uint32_t reset_status,
    const char *format,
    ...)
{
    va_list arguments;

    if (diagnostic == NULL) {
        return;
    }
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = status;
    diagnostic->gl_code = gl_code;
    diagnostic->reset_status = reset_status;
    if (format == NULL) {
        return;
    }
    va_start(arguments, format);
    vsnprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        format,
        arguments);
    va_end(arguments);
}

static bool copy_function_pointer(
    void *destination,
    size_t destination_size,
    boxrig_gl_function source)
{
    if (source == NULL || destination_size != sizeof(source)) {
        return false;
    }
    memcpy(destination, &source, destination_size);
    return true;
}

#define LOAD_GL(runtime, member, name, diagnostic, failure_label)               \
    do {                                                                        \
        boxrig_gl_function resolved_function =                                  \
            (runtime)->config.resolve(                                          \
                (runtime)->config.resolver_context,                             \
                (name));                                                        \
        if (!copy_function_pointer(                                             \
                &(runtime)->gl.member,                                          \
                sizeof((runtime)->gl.member),                                   \
                resolved_function)) {                                           \
            set_diagnostic(                                                     \
                (diagnostic),                                                   \
                BOXRIG_GL_UNAVAILABLE,                                          \
                0,                                                              \
                0,                                                              \
                "required OpenGL entry point %s is unavailable",               \
                (name));                                                        \
            goto failure_label;                                                 \
        }                                                                       \
    } while (0)

static bool parse_version(
    const char *version,
    uint32_t *major_out,
    uint32_t *minor_out,
    bool *is_gles_out)
{
    unsigned int major;
    unsigned int minor;

    if (version == NULL) return false;
    *is_gles_out = strncmp(version, "OpenGL ES ", 10) == 0;
    if (*is_gles_out) version += 10;
    if (sscanf(version, "%u.%u", &major, &minor) != 2) {
        return false;
    }
    *major_out = major;
    *minor_out = minor;
    return true;
}

static bool has_extension(
    const boxrig_gl_runtime *runtime,
    const char *required)
{
    GLint count = 0;
    GLint index;

    runtime->gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (index = 0; index < count; index++) {
        const char *extension = (const char *)runtime->gl.GetStringi(
            GL_EXTENSIONS,
            (GLuint)index);

        if (extension != NULL && strcmp(extension, required) == 0) {
            return true;
        }
    }
    return false;
}

static bool current_context_matches(
    const boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic)
{
    boxrig_gl_current_context current;

    if (context_generation != runtime->config.context_generation) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_STALE_GENERATION,
            0,
            0,
            "context generation is stale");
        return false;
    }
    memset(&current, 0, sizeof(current));
    if (!runtime->config.query_current_context(
            runtime->config.current_context_query_context,
            &current) ||
        current.identity == 0 ||
        current.identity != runtime->bound_context.identity ||
        current.thread_identity != runtime->bound_context.thread_identity) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_CONTEXT_MISMATCH,
            0,
            0,
            "the bound OpenGL context is not current on its owner thread");
        return false;
    }
    return true;
}

static void set_enabled(
    const boxrig_gl_api *gl,
    GLenum capability,
    GLboolean enabled)
{
    if (enabled) {
        gl->Enable(capability);
    } else {
        gl->Disable(capability);
    }
}

static void save_state(boxrig_gl_runtime *runtime)
{
    boxrig_gl_saved_state *state = &runtime->saved_state;
    const boxrig_gl_api *gl = &runtime->gl;

    memset(state, 0, sizeof(*state));
    gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
    gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_framebuffer);
    gl->GetIntegerv(GL_VIEWPORT, state->viewport);
    gl->GetIntegerv(GL_SCISSOR_BOX, state->scissor_box);
    state->scissor_enabled = gl->IsEnabled(GL_SCISSOR_TEST);
    gl->GetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
    state->blend_enabled = gl->IsEnabled(GL_BLEND);
    gl->GetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_equation_rgb);
    gl->GetIntegerv(GL_BLEND_EQUATION_ALPHA, &state->blend_equation_alpha);
    gl->GetIntegerv(GL_BLEND_SRC_RGB, &state->blend_source_rgb);
    gl->GetIntegerv(GL_BLEND_SRC_ALPHA, &state->blend_source_alpha);
    gl->GetIntegerv(GL_BLEND_DST_RGB, &state->blend_destination_rgb);
    gl->GetIntegerv(GL_BLEND_DST_ALPHA, &state->blend_destination_alpha);
    gl->GetFloatv(GL_BLEND_COLOR, state->blend_color);
    state->depth_enabled = gl->IsEnabled(GL_DEPTH_TEST);
    gl->GetBooleanv(GL_DEPTH_WRITEMASK, &state->depth_mask);
    state->stencil_enabled = gl->IsEnabled(GL_STENCIL_TEST);
    state->cull_enabled = gl->IsEnabled(GL_CULL_FACE);
    if (!runtime->capabilities.is_gles)
        state->multisample_enabled = gl->IsEnabled(GL_MULTISAMPLE);
    if (runtime->srgb_write_control)
        state->framebuffer_srgb_enabled = gl->IsEnabled(GL_FRAMEBUFFER_SRGB);
    gl->GetIntegerv(GL_CURRENT_PROGRAM, &state->program);
    gl->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vertex_array);
    gl->GetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
    gl->GetIntegerv(
        GL_PIXEL_UNPACK_BUFFER_BINDING,
        &state->pixel_unpack_buffer);
    gl->GetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture_2d);
    gl->GetIntegerv(GL_SAMPLER_BINDING, &state->sampler);
    gl->ActiveTexture((GLenum)state->active_texture);
    gl->GetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
    gl->GetIntegerv(GL_UNPACK_ROW_LENGTH, &state->unpack_row_length);
    gl->GetIntegerv(GL_UNPACK_SKIP_ROWS, &state->unpack_skip_rows);
    gl->GetIntegerv(GL_UNPACK_SKIP_PIXELS, &state->unpack_skip_pixels);
    runtime->counters.query_count += 29 + !runtime->capabilities.is_gles + runtime->srgb_write_control;
    runtime->counters.change_count += 2;
    runtime->counters.restore_count++;
}

static void restore_state(boxrig_gl_runtime *runtime)
{
    const boxrig_gl_saved_state *state = &runtime->saved_state;
    const boxrig_gl_api *gl = &runtime->gl;

    gl->BindFramebuffer(
        GL_DRAW_FRAMEBUFFER,
        (GLuint)state->draw_framebuffer);
    gl->BindFramebuffer(
        GL_READ_FRAMEBUFFER,
        (GLuint)state->read_framebuffer);
    gl->Viewport(
        state->viewport[0],
        state->viewport[1],
        state->viewport[2],
        state->viewport[3]);
    gl->Scissor(
        state->scissor_box[0],
        state->scissor_box[1],
        state->scissor_box[2],
        state->scissor_box[3]);
    set_enabled(gl, GL_SCISSOR_TEST, state->scissor_enabled);
    gl->ColorMask(
        state->color_mask[0],
        state->color_mask[1],
        state->color_mask[2],
        state->color_mask[3]);
    gl->BlendEquationSeparate(
        (GLenum)state->blend_equation_rgb,
        (GLenum)state->blend_equation_alpha);
    gl->BlendFuncSeparate(
        (GLenum)state->blend_source_rgb,
        (GLenum)state->blend_destination_rgb,
        (GLenum)state->blend_source_alpha,
        (GLenum)state->blend_destination_alpha);
    gl->BlendColor(
        state->blend_color[0],
        state->blend_color[1],
        state->blend_color[2],
        state->blend_color[3]);
    set_enabled(gl, GL_BLEND, state->blend_enabled);
    gl->DepthMask(state->depth_mask);
    set_enabled(gl, GL_DEPTH_TEST, state->depth_enabled);
    set_enabled(gl, GL_STENCIL_TEST, state->stencil_enabled);
    set_enabled(gl, GL_CULL_FACE, state->cull_enabled);
    if (!runtime->capabilities.is_gles)
        set_enabled(gl, GL_MULTISAMPLE, state->multisample_enabled);
    if (runtime->srgb_write_control)
        set_enabled(gl, GL_FRAMEBUFFER_SRGB, state->framebuffer_srgb_enabled);
    gl->UseProgram((GLuint)state->program);
    gl->BindVertexArray((GLuint)state->vertex_array);
    gl->BindBuffer(GL_ARRAY_BUFFER, (GLuint)state->array_buffer);
    gl->BindBuffer(
        GL_PIXEL_UNPACK_BUFFER,
        (GLuint)state->pixel_unpack_buffer);
    gl->ActiveTexture(GL_TEXTURE0);
    gl->BindTexture(GL_TEXTURE_2D, (GLuint)state->texture_2d);
    gl->BindSampler(0, (GLuint)state->sampler);
    gl->ActiveTexture((GLenum)state->active_texture);
    gl->PixelStorei(GL_UNPACK_ALIGNMENT, state->unpack_alignment);
    gl->PixelStorei(GL_UNPACK_ROW_LENGTH, state->unpack_row_length);
    gl->PixelStorei(GL_UNPACK_SKIP_ROWS, state->unpack_skip_rows);
    gl->PixelStorei(GL_UNPACK_SKIP_PIXELS, state->unpack_skip_pixels);
    runtime->counters.restore_count += 27 + !runtime->capabilities.is_gles + runtime->srgb_write_control;
}

static void clear_errors(const boxrig_gl_api *gl)
{
    size_t count;

    for (count = 0; count < 16 && gl->GetError() != GL_NO_ERROR; count++) {
    }
}

static void delete_resources(boxrig_gl_runtime *runtime)
{
    if (runtime->sampler != 0) {
        runtime->gl.DeleteSamplers(1, &runtime->sampler);
        runtime->sampler = 0;
    }
    if (runtime->texture != 0) {
        runtime->gl.DeleteTextures(1, &runtime->texture);
        runtime->texture = 0;
    }
    if (runtime->buffers[0] != 0 || runtime->buffers[1] != 0) {
        runtime->gl.DeleteBuffers(2, runtime->buffers);
        runtime->buffers[0] = 0;
        runtime->buffers[1] = 0;
    }
    if (runtime->vertex_array != 0) {
        runtime->gl.DeleteVertexArrays(1, &runtime->vertex_array);
        runtime->vertex_array = 0;
    }
    if (runtime->program != 0) {
        runtime->gl.DeleteProgram(runtime->program);
        runtime->program = 0;
    }
}

static boxrig_gl_status compile_probe_shader(
    boxrig_gl_runtime *runtime,
    GLenum type,
    const char *source,
    GLuint *shader_out,
    boxrig_gl_diagnostic *diagnostic)
{
    GLuint shader;
    GLint compiled = GL_FALSE;
    GLsizei length = 0;
    char log[384];

    shader = runtime->gl.CreateShader(type);
    if (shader == 0) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_API_FAILURE,
            runtime->gl.GetError(),
            0,
            "OpenGL failed to create a probe shader");
        return BOXRIG_GL_API_FAILURE;
    }
    const char *sources[] = {
        runtime->capabilities.is_gles
            ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
            : "#version 330 core\n",
        source};
    runtime->gl.ShaderSource(shader, 2, sources, NULL);
    runtime->gl.CompileShader(shader);
    runtime->gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        memset(log, 0, sizeof(log));
        runtime->gl.GetShaderInfoLog(
            shader,
            (GLsizei)sizeof(log),
            &length,
            log);
        log[sizeof(log) - 1] = '\0';
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_SHADER_FAILURE,
            0,
            0,
            "probe shader compilation failed: %s",
            log);
        runtime->gl.DeleteShader(shader);
        return BOXRIG_GL_SHADER_FAILURE;
    }
    *shader_out = shader;
    return BOXRIG_GL_OK;
}

static boxrig_gl_status create_resources(
    boxrig_gl_runtime *runtime,
    boxrig_gl_diagnostic *diagnostic)
{
    static const char vertex_source[] =
        "layout(location=0) in vec4 destination;\n"
        "layout(location=1) in vec4 texture_box;\n"
        "layout(location=2) in vec4 tint;\n"
        "layout(location=3) in float source_kind;\n"
        "out vec2 texture_position;\n"
        "out vec4 fragment_tint;\n"
        "flat out int fragment_source_kind;\n"
        "void main() {\n"
        "  vec2 corner = vec2(float((gl_VertexID >> 1) & 1), "
        "float(gl_VertexID & 1));\n"
        "  gl_Position = vec4(mix(destination.xy, destination.zw, corner), "
        "0.0, 1.0);\n"
        "  texture_position = mix(texture_box.xy, texture_box.zw, corner);\n"
        "  fragment_tint = tint;\n"
        "  fragment_source_kind = int(source_kind + 0.5);\n"
        "}\n";
    static const char fragment_source[] =
        "uniform sampler2D atlas;\n"
        "in vec2 texture_position;\n"
        "in vec4 fragment_tint;\n"
        "flat in int fragment_source_kind;\n"
        "out vec4 color;\n"
        "void main() {\n"
        "  if (fragment_source_kind == 0) { color = fragment_tint; return; }\n"
        "  vec4 sample_value = texture(atlas, texture_position);\n"
        "  if (fragment_source_kind == 1) {\n"
        "    float alpha = sample_value.r * fragment_tint.a;\n"
        "    color = vec4(fragment_tint.rgb * alpha, alpha);\n"
        "  } else {\n"
        "    color = sample_value * fragment_tint;\n"
        "  }\n"
        "}\n";
    GLuint vertex = 0;
    GLuint fragment = 0;
    GLint linked = GL_FALSE;
    GLsizei length = 0;
    char log[384];
    boxrig_gl_status status;
    GLenum error;
    GLint active_texture;
    GLint texture_binding;
    GLint pixel_unpack_buffer;
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint unpack_skip_rows;
    GLint unpack_skip_pixels;

    clear_errors(&runtime->gl);
    status = compile_probe_shader(
        runtime,
        GL_VERTEX_SHADER,
        vertex_source,
        &vertex,
        diagnostic);
    if (status != BOXRIG_GL_OK) {
        return status;
    }
    status = compile_probe_shader(
        runtime,
        GL_FRAGMENT_SHADER,
        fragment_source,
        &fragment,
        diagnostic);
    if (status != BOXRIG_GL_OK) {
        runtime->gl.DeleteShader(vertex);
        return status;
    }
    runtime->program = runtime->gl.CreateProgram();
    if (runtime->program == 0) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_API_FAILURE,
            runtime->gl.GetError(),
            0,
            "OpenGL failed to create the probe program");
        runtime->gl.DeleteShader(fragment);
        runtime->gl.DeleteShader(vertex);
        return BOXRIG_GL_API_FAILURE;
    }
    runtime->gl.AttachShader(runtime->program, vertex);
    runtime->gl.AttachShader(runtime->program, fragment);
    runtime->gl.LinkProgram(runtime->program);
    runtime->gl.GetProgramiv(runtime->program, GL_LINK_STATUS, &linked);
    runtime->gl.DeleteShader(fragment);
    runtime->gl.DeleteShader(vertex);
    if (linked != GL_TRUE) {
        memset(log, 0, sizeof(log));
        runtime->gl.GetProgramInfoLog(
            runtime->program,
            (GLsizei)sizeof(log),
            &length,
            log);
        log[sizeof(log) - 1] = '\0';
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_SHADER_FAILURE,
            0,
            0,
            "probe program link failed: %s",
            log);
        delete_resources(runtime);
        return BOXRIG_GL_SHADER_FAILURE;
    }
    runtime->gl.GenVertexArrays(1, &runtime->vertex_array);
    runtime->gl.GenBuffers(2, runtime->buffers);
    runtime->gl.GenTextures(1, &runtime->texture);
    runtime->gl.GenSamplers(1, &runtime->sampler);
    error = runtime->gl.GetError();
    if (error != GL_NO_ERROR || runtime->vertex_array == 0 ||
        runtime->buffers[0] == 0 || runtime->buffers[1] == 0 ||
        runtime->texture == 0 || runtime->sampler == 0) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL failed to allocate probe resources");
        delete_resources(runtime);
        return BOXRIG_GL_API_FAILURE;
    }
    runtime->gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    runtime->gl.ActiveTexture(GL_TEXTURE0);
    runtime->gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture_binding);
    runtime->gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pixel_unpack_buffer);
    runtime->gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    runtime->gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    runtime->gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    runtime->gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
    runtime->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    runtime->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    runtime->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    runtime->gl.BindTexture(GL_TEXTURE_2D, runtime->texture);
    {
        static const uint8_t white[] = {255, 255, 255, 255};

        runtime->gl.TexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            1,
            1,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            white);
    }
    runtime->gl.SamplerParameteri(
        runtime->sampler,
        GL_TEXTURE_MIN_FILTER,
        GL_LINEAR);
    runtime->gl.SamplerParameteri(
        runtime->sampler,
        GL_TEXTURE_MAG_FILTER,
        GL_LINEAR);
    runtime->gl.SamplerParameteri(
        runtime->sampler,
        GL_TEXTURE_WRAP_S,
        GL_CLAMP_TO_EDGE);
    runtime->gl.SamplerParameteri(
        runtime->sampler,
        GL_TEXTURE_WRAP_T,
        GL_CLAMP_TO_EDGE);
    error = runtime->gl.GetError();
    runtime->gl.BindTexture(GL_TEXTURE_2D, (GLuint)texture_binding);
    runtime->gl.BindBuffer(
        GL_PIXEL_UNPACK_BUFFER,
        (GLuint)pixel_unpack_buffer);
    runtime->gl.PixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    runtime->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
    runtime->gl.ActiveTexture((GLenum)active_texture);
    if (error != GL_NO_ERROR) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL failed to initialize probe resources");
        delete_resources(runtime);
        return BOXRIG_GL_API_FAILURE;
    }
    return BOXRIG_GL_OK;
}

static boxrig_gl_status validate_target_properties(
    boxrig_gl_runtime *runtime,
    const boxrig_gl_target *target,
    boxrig_gl_diagnostic *diagnostic)
{
    GLint samples = 0;

    runtime->gl.GetIntegerv(GL_SAMPLES, &samples);
    runtime->counters.query_count++;
    if (samples < 0 || (uint32_t)samples != target->samples) {
        set_diagnostic(
            diagnostic,
            BOXRIG_GL_TARGET_MISMATCH,
            (uint32_t)samples,
            0,
            "target sample count differs from the borrowed descriptor");
        return BOXRIG_GL_TARGET_MISMATCH;
    }
    if (target->draw_framebuffer != 0) {
        static const GLenum size_names[] = {
            GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,
            GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE};
        GLint sizes[4];
        GLint encoding;
        GLint component_type;
        size_t index;

        for (index = 0; index < 4; index++) {
            runtime->gl.GetFramebufferAttachmentParameteriv(
                GL_DRAW_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                size_names[index],
                &sizes[index]);
        }
        runtime->gl.GetFramebufferAttachmentParameteriv(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
            &encoding);
        runtime->gl.GetFramebufferAttachmentParameteriv(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,
            &component_type);
        runtime->counters.query_count += 6;
        if (sizes[0] != 8 || sizes[1] != 8 || sizes[2] != 8 ||
            sizes[3] != 8 || component_type != GL_UNSIGNED_NORMALIZED ||
            (target->format == BOXRIG_GL_TARGET_RGBA8 &&
             encoding != GL_LINEAR) ||
            (target->format == BOXRIG_GL_TARGET_SRGB8_ALPHA8 &&
             encoding != GL_SRGB)) {
            set_diagnostic(
                diagnostic,
                BOXRIG_GL_TARGET_MISMATCH,
                (uint32_t)encoding,
                0,
                "target attachment is not the declared RGBA8 encoding");
            return BOXRIG_GL_TARGET_MISMATCH;
        }
    }
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_create(
    const boxrig_gl_runtime_config *config,
    boxrig_gl_runtime **runtime_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_gl_runtime *runtime;
    const char *version;
    GLint profile = 0;
    GLint maximum_samples = 0;
    boxrig_gl_status status = BOXRIG_GL_UNAVAILABLE;
    boxrig_gl_function reset_function;
    const char *reset_name = NULL;

    if (runtime_out == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime output is required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *runtime_out = NULL;
    if (config == NULL || config->resolve == NULL ||
        config->query_current_context == NULL ||
        config->context_generation == 0 || config->target_generation == 0) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "resolver, current-context query, and generations are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_OUT_OF_MEMORY,
            0,
            0,
            "could not allocate the OpenGL runtime");
        return BOXRIG_GL_OUT_OF_MEMORY;
    }
    runtime->config = *config;
    runtime->target_generation = config->target_generation;
    if (!config->query_current_context(
            config->current_context_query_context,
            &runtime->bound_context) ||
        runtime->bound_context.identity == 0 ||
        runtime->bound_context.thread_identity == 0) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_CONTEXT_MISMATCH,
            0,
            0,
            "an OpenGL context must be current at runtime creation");
        status = BOXRIG_GL_CONTEXT_MISMATCH;
        goto fail;
    }
    LOAD_GL(runtime, GetString, "glGetString", diagnostic_out, fail);
    LOAD_GL(runtime, GetStringi, "glGetStringi", diagnostic_out, fail);
    LOAD_GL(runtime, GetIntegerv, "glGetIntegerv", diagnostic_out, fail);
    version = (const char *)runtime->gl.GetString(GL_VERSION);
    if (!parse_version(
            version,
            &runtime->capabilities.major_version,
            &runtime->capabilities.minor_version,
            &runtime->capabilities.is_gles) ||
        runtime->capabilities.major_version < 3 ||
        (!runtime->capabilities.is_gles && runtime->capabilities.major_version == 3 &&
         runtime->capabilities.minor_version < 3)) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_UNAVAILABLE,
            0,
            0,
            "OpenGL 3.3 core or OpenGL ES 3.0 or newer is required; found %s",
            version == NULL ? "no version" : version);
        goto fail;
    }
    if (!runtime->capabilities.is_gles)
        runtime->gl.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    if (!runtime->capabilities.is_gles && (profile & GL_CONTEXT_CORE_PROFILE_BIT) == 0) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_UNAVAILABLE,
            0,
            0,
            "an OpenGL core profile context is required");
        goto fail;
    }
    runtime->capabilities.core_profile = !runtime->capabilities.is_gles;
    /* ES always performs sRGB conversion for sRGB attachments unless this
     * extension exposes the desktop enable. MULTISAMPLE has no ES enable. */
    runtime->srgb_write_control = !runtime->capabilities.is_gles ||
        has_extension(runtime, "GL_EXT_sRGB_write_control");
    {
        const char *vendor = (const char *)runtime->gl.GetString(GL_VENDOR);
        const char *renderer = (const char *)runtime->gl.GetString(GL_RENDERER);
        snprintf(runtime->capabilities.vendor, sizeof(runtime->capabilities.vendor),
            "%.*s", (int)sizeof(runtime->capabilities.vendor) - 1, vendor == NULL ? "" : vendor);
        snprintf(runtime->capabilities.renderer, sizeof(runtime->capabilities.renderer),
            "%.*s", (int)sizeof(runtime->capabilities.renderer) - 1, renderer == NULL ? "" : renderer);
        snprintf(runtime->capabilities.version, sizeof(runtime->capabilities.version),
            "%.*s", (int)sizeof(runtime->capabilities.version) - 1, version);
    }
    LOAD_GL(runtime, GetBooleanv, "glGetBooleanv", diagnostic_out, fail);
    LOAD_GL(runtime, GetFloatv, "glGetFloatv", diagnostic_out, fail);
    LOAD_GL(runtime, IsEnabled, "glIsEnabled", diagnostic_out, fail);
    LOAD_GL(runtime, Enable, "glEnable", diagnostic_out, fail);
    LOAD_GL(runtime, Disable, "glDisable", diagnostic_out, fail);
    LOAD_GL(runtime, Viewport, "glViewport", diagnostic_out, fail);
    LOAD_GL(runtime, Scissor, "glScissor", diagnostic_out, fail);
    LOAD_GL(runtime, ColorMask, "glColorMask", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        BlendEquationSeparate,
        "glBlendEquationSeparate",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        BlendFuncSeparate,
        "glBlendFuncSeparate",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, BlendColor, "glBlendColor", diagnostic_out, fail);
    LOAD_GL(runtime, DepthMask, "glDepthMask", diagnostic_out, fail);
    LOAD_GL(runtime, UseProgram, "glUseProgram", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        BindVertexArray,
        "glBindVertexArray",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, BindBuffer, "glBindBuffer", diagnostic_out, fail);
    LOAD_GL(runtime, ActiveTexture, "glActiveTexture", diagnostic_out, fail);
    LOAD_GL(runtime, BindTexture, "glBindTexture", diagnostic_out, fail);
    LOAD_GL(runtime, BindSampler, "glBindSampler", diagnostic_out, fail);
    LOAD_GL(runtime, PixelStorei, "glPixelStorei", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        BindFramebuffer,
        "glBindFramebuffer",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        CheckFramebufferStatus,
        "glCheckFramebufferStatus",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        GetFramebufferAttachmentParameteriv,
        "glGetFramebufferAttachmentParameteriv",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, GetError, "glGetError", diagnostic_out, fail);
    LOAD_GL(runtime, CreateShader, "glCreateShader", diagnostic_out, fail);
    LOAD_GL(runtime, ShaderSource, "glShaderSource", diagnostic_out, fail);
    LOAD_GL(runtime, CompileShader, "glCompileShader", diagnostic_out, fail);
    LOAD_GL(runtime, GetShaderiv, "glGetShaderiv", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        GetShaderInfoLog,
        "glGetShaderInfoLog",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, DeleteShader, "glDeleteShader", diagnostic_out, fail);
    LOAD_GL(runtime, CreateProgram, "glCreateProgram", diagnostic_out, fail);
    LOAD_GL(runtime, AttachShader, "glAttachShader", diagnostic_out, fail);
    LOAD_GL(runtime, LinkProgram, "glLinkProgram", diagnostic_out, fail);
    LOAD_GL(runtime, GetProgramiv, "glGetProgramiv", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        GetProgramInfoLog,
        "glGetProgramInfoLog",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, DeleteProgram, "glDeleteProgram", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        GenVertexArrays,
        "glGenVertexArrays",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        DeleteVertexArrays,
        "glDeleteVertexArrays",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, GenBuffers, "glGenBuffers", diagnostic_out, fail);
    LOAD_GL(runtime, DeleteBuffers, "glDeleteBuffers", diagnostic_out, fail);
    LOAD_GL(runtime, BufferData, "glBufferData", diagnostic_out, fail);
    LOAD_GL(runtime, BufferSubData, "glBufferSubData", diagnostic_out, fail);
    LOAD_GL(runtime, GenTextures, "glGenTextures", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        DeleteTextures,
        "glDeleteTextures",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, TexImage2D, "glTexImage2D", diagnostic_out, fail);
    LOAD_GL(runtime, TexSubImage2D, "glTexSubImage2D", diagnostic_out, fail);
    LOAD_GL(runtime, TexParameteri, "glTexParameteri", diagnostic_out, fail);
    LOAD_GL(runtime, GenSamplers, "glGenSamplers", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        DeleteSamplers,
        "glDeleteSamplers",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        SamplerParameteri,
        "glSamplerParameteri",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        FramebufferTexture2D,
        "glFramebufferTexture2D",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        GetUniformLocation,
        "glGetUniformLocation",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, Uniform1i, "glUniform1i", diagnostic_out, fail);
    LOAD_GL(runtime, Uniform4fv, "glUniform4fv", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        VertexAttribPointer,
        "glVertexAttribPointer",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        EnableVertexAttribArray,
        "glEnableVertexAttribArray",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        VertexAttribDivisor,
        "glVertexAttribDivisor",
        diagnostic_out,
        fail);
    LOAD_GL(
        runtime,
        DrawArraysInstanced,
        "glDrawArraysInstanced",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, FenceSync, "glFenceSync", diagnostic_out, fail);
    LOAD_GL(
        runtime,
        ClientWaitSync,
        "glClientWaitSync",
        diagnostic_out,
        fail);
    LOAD_GL(runtime, DeleteSync, "glDeleteSync", diagnostic_out, fail);

    runtime->gl.GetIntegerv(GL_MAX_SAMPLES, &maximum_samples);
    runtime->capabilities.maximum_samples = maximum_samples > 0
                                                ? (uint32_t)maximum_samples
                                                : 0;
    runtime->capabilities.srgb_target = true;
    bool es32 = runtime->capabilities.is_gles &&
        (runtime->capabilities.major_version > 3 || runtime->capabilities.minor_version >= 2);
    bool desktop43 = !runtime->capabilities.is_gles &&
        (runtime->capabilities.major_version > 4 ||
         (runtime->capabilities.major_version == 4 && runtime->capabilities.minor_version >= 3));
    bool desktop45 = !runtime->capabilities.is_gles &&
        (runtime->capabilities.major_version > 4 ||
         (runtime->capabilities.major_version == 4 && runtime->capabilities.minor_version >= 5));
    if (es32 || desktop45 ||
        (!runtime->capabilities.is_gles && has_extension(runtime, "GL_KHR_robustness"))) {
        reset_name = "glGetGraphicsResetStatus";
    } else if (runtime->capabilities.is_gles && has_extension(runtime, "GL_KHR_robustness")) {
        reset_name = "glGetGraphicsResetStatusKHR";
    } else if (has_extension(runtime, "GL_ARB_robustness")) {
        reset_name = "glGetGraphicsResetStatusARB";
    }
    reset_function = reset_name == NULL ? NULL :
        config->resolve(config->resolver_context, reset_name);
    if (reset_function != NULL &&
        copy_function_pointer(
            &runtime->gl.GetGraphicsResetStatus,
            sizeof(runtime->gl.GetGraphicsResetStatus),
            reset_function)) {
        runtime->capabilities.robustness = true;
        snprintf(runtime->capabilities.reset_status_entry_point,
            sizeof(runtime->capabilities.reset_status_entry_point), "%s", reset_name);
    }
    runtime->capabilities.debug_output =
        (es32 || desktop43 ||
         (!runtime->capabilities.is_gles && has_extension(runtime, "GL_KHR_debug"))) &&
        config->resolve(config->resolver_context, "glGetDebugMessageLog") != NULL;
    status = create_resources(runtime, diagnostic_out);
    if (status != BOXRIG_GL_OK) {
        goto fail;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "OpenGL runtime is available");
    *runtime_out = runtime;
    return BOXRIG_GL_OK;

fail:
    free(runtime);
    return status;
}

boxrig_gl_status boxrig_gl_runtime_get_capabilities(
    const boxrig_gl_runtime *runtime,
    boxrig_gl_capabilities *capabilities_out)
{
    if (runtime == NULL || capabilities_out == NULL) {
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *capabilities_out = runtime->capabilities;
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_set_target_generation(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    uint64_t target_generation,
    boxrig_gl_diagnostic *diagnostic_out)
{
    if (runtime == NULL || target_generation == 0) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime and nonzero target generation are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    if (runtime->target_active) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_BUSY,
            0,
            0,
            "target generation cannot change during a frame");
        return BOXRIG_GL_BUSY;
    }
    if (target_generation <= runtime->target_generation) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_STALE_GENERATION,
            0,
            0,
            "target generation must advance monotonically");
        return BOXRIG_GL_STALE_GENERATION;
    }
    runtime->target_generation = target_generation;
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "target generation advanced");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_begin_target(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    const boxrig_gl_target *target,
    const boxrig_gl_clip *clip,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_gl_status status;
    GLenum framebuffer_status;
    GLenum error;
    GLenum reset_status = GL_NO_ERROR;

    if (runtime == NULL || target == NULL || clip == NULL ||
        target->width == 0 || target->height == 0 ||
        target->width > INT_MAX || target->height > INT_MAX ||
        clip->width == 0 || clip->height == 0 || clip->x > target->width ||
        clip->y > target->height || clip->width > target->width - clip->x ||
        clip->height > target->height - clip->y) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "target and clip extents must be nonempty and in range");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    if (runtime->target_active) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_BUSY,
            0,
            0,
            "one borrowed target is already active");
        return BOXRIG_GL_BUSY;
    }
    if (target->generation != runtime->target_generation) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_STALE_GENERATION,
            0,
            0,
            "borrowed target generation is stale");
        return BOXRIG_GL_STALE_GENERATION;
    }
    if (target->format < BOXRIG_GL_TARGET_RGBA8 ||
        target->format > BOXRIG_GL_TARGET_SRGB8_ALPHA8 ||
        target->alpha_mode < BOXRIG_GL_ALPHA_OPAQUE ||
        target->alpha_mode > BOXRIG_GL_ALPHA_PREMULTIPLIED ||
        target->origin != BOXRIG_GL_TARGET_ORIGIN_LOWER_LEFT ||
        target->samples > runtime->capabilities.maximum_samples) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_TARGET_MISMATCH,
            0,
            0,
            "borrowed target format, origin, alpha, or samples are unsupported");
        return BOXRIG_GL_TARGET_MISMATCH;
    }
    if (runtime->gl.GetGraphicsResetStatus != NULL) {
        reset_status = runtime->gl.GetGraphicsResetStatus();
        if (reset_status != GL_NO_ERROR) {
            set_diagnostic(
                diagnostic_out,
                BOXRIG_GL_API_FAILURE,
                0,
                reset_status,
                "the OpenGL context reported a graphics reset");
            return BOXRIG_GL_API_FAILURE;
        }
    }

    save_state(runtime);
    clear_errors(&runtime->gl);
    runtime->gl.BindFramebuffer(GL_FRAMEBUFFER, target->draw_framebuffer);
    runtime->gl.Viewport(0, 0, (GLsizei)target->width, (GLsizei)target->height);
    runtime->gl.Enable(GL_SCISSOR_TEST);
    runtime->gl.Scissor(
        (GLint)clip->x,
        (GLint)(target->height - clip->y - clip->height),
        (GLsizei)clip->width,
        (GLsizei)clip->height);
    runtime->gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    runtime->gl.Enable(GL_BLEND);
    runtime->gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    runtime->gl.BlendFuncSeparate(
        GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA,
        GL_ONE,
        GL_ONE_MINUS_SRC_ALPHA);
    runtime->gl.BlendColor(0.0f, 0.0f, 0.0f, 0.0f);
    runtime->gl.Disable(GL_DEPTH_TEST);
    runtime->gl.DepthMask(GL_FALSE);
    runtime->gl.Disable(GL_STENCIL_TEST);
    runtime->gl.Disable(GL_CULL_FACE);
    if (!runtime->capabilities.is_gles)
        set_enabled(&runtime->gl, GL_MULTISAMPLE, target->samples > 1 ? GL_TRUE : GL_FALSE);
    if (runtime->srgb_write_control)
        set_enabled(&runtime->gl, GL_FRAMEBUFFER_SRGB,
            target->format == BOXRIG_GL_TARGET_SRGB8_ALPHA8 ? GL_TRUE : GL_FALSE);
    runtime->gl.UseProgram(runtime->program);
    runtime->gl.BindVertexArray(runtime->vertex_array);
    runtime->gl.BindBuffer(GL_ARRAY_BUFFER, runtime->buffers[0]);
    runtime->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, runtime->buffers[1]);
    runtime->gl.ActiveTexture(GL_TEXTURE0);
    runtime->gl.BindTexture(GL_TEXTURE_2D, runtime->texture);
    runtime->gl.BindSampler(0, runtime->sampler);
    runtime->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    runtime->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    runtime->counters.change_count += 27 + !runtime->capabilities.is_gles + runtime->srgb_write_control;

    framebuffer_status = runtime->gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    runtime->counters.query_count++;
    if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
        restore_state(runtime);
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_TARGET_INCOMPLETE,
            framebuffer_status,
            0,
            "borrowed framebuffer is incomplete (0x%04x)",
            framebuffer_status);
        return BOXRIG_GL_TARGET_INCOMPLETE;
    }
    status = validate_target_properties(runtime, target, diagnostic_out);
    if (status != BOXRIG_GL_OK) {
        restore_state(runtime);
        return status;
    }
    error = runtime->gl.GetError();
    if (error != GL_NO_ERROR) {
        restore_state(runtime);
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL target setup failed (0x%04x)",
            error);
        return BOXRIG_GL_API_FAILURE;
    }
    runtime->target_active = true;
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "borrowed target is active");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_end_target(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLenum frame_error;
    GLenum restore_error;

    if (runtime == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime is required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    if (!runtime->target_active) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "no borrowed target is active");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    frame_error = runtime->gl.GetError();
    restore_state(runtime);
    runtime->target_active = false;
    restore_error = runtime->gl.GetError();
    if (frame_error != GL_NO_ERROR || restore_error != GL_NO_ERROR) {
        GLenum error = frame_error != GL_NO_ERROR ? frame_error : restore_error;

        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL frame or state restoration failed (0x%04x)",
            error);
        return BOXRIG_GL_API_FAILURE;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "caller OpenGL state was restored");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_create_texture(
    boxrig_gl_runtime *runtime,
    uint32_t width,
    uint32_t height,
    boxrig_gl_texture_format format,
    uint32_t *texture_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLuint texture = 0;
    GLenum internal_format;
    GLenum upload_format;
    GLenum error;

    if (runtime == NULL || !runtime->target_active || texture_out == NULL ||
        width == 0 || height == 0 || width > INT_MAX || height > INT_MAX ||
        format < BOXRIG_GL_TEXTURE_COVERAGE8 ||
        format > BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "an active target and valid texture descriptor are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *texture_out = 0;
    internal_format = format == BOXRIG_GL_TEXTURE_COVERAGE8 ? GL_R8 : GL_RGBA8;
    upload_format = format == BOXRIG_GL_TEXTURE_COVERAGE8 ? GL_RED : GL_RGBA;
    runtime->gl.GenTextures(1, &texture);
    runtime->gl.BindTexture(GL_TEXTURE_2D, texture);
    runtime->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    runtime->gl.TexImage2D(
        GL_TEXTURE_2D,
        0,
        (GLint)internal_format,
        (GLsizei)width,
        (GLsizei)height,
        0,
        upload_format,
        GL_UNSIGNED_BYTE,
        NULL);
    runtime->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    runtime->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    runtime->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    runtime->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    error = runtime->gl.GetError();
    if (error != GL_NO_ERROR || texture == 0) {
        if (texture != 0) {
            runtime->gl.DeleteTextures(1, &texture);
        }
        set_diagnostic(
            diagnostic_out,
            error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                      : BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL texture allocation failed");
        return error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                         : BOXRIG_GL_API_FAILURE;
    }
    *texture_out = texture;
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "renderer texture storage was allocated");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_upload_texture(
    boxrig_gl_runtime *runtime,
    uint32_t texture,
    boxrig_gl_texture_format format,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const uint8_t *bytes,
    size_t byte_count,
    boxrig_gl_diagnostic *diagnostic_out)
{
    size_t bytes_per_pixel;
    size_t expected;
    GLenum upload_format;
    GLenum error;

    bytes_per_pixel = format == BOXRIG_GL_TEXTURE_COVERAGE8 ? 1u : 4u;
    if (runtime == NULL || !runtime->target_active || texture == 0 ||
        bytes == NULL || width == 0 || height == 0 || width > INT_MAX ||
        height > INT_MAX || x > INT_MAX || y > INT_MAX ||
        format < BOXRIG_GL_TEXTURE_COVERAGE8 ||
        format > BOXRIG_GL_TEXTURE_PREMULTIPLIED_RGBA8 ||
        width > SIZE_MAX / height ||
        width * (size_t)height > SIZE_MAX / bytes_per_pixel) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "an active target and bounded texture upload are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    expected = width * (size_t)height * bytes_per_pixel;
    if (byte_count != expected) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "texture upload byte count does not match its rectangle");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    upload_format = format == BOXRIG_GL_TEXTURE_COVERAGE8 ? GL_RED : GL_RGBA;
    runtime->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    runtime->gl.BindTexture(GL_TEXTURE_2D, texture);
    runtime->gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    runtime->gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    runtime->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    runtime->gl.TexSubImage2D(
        GL_TEXTURE_2D,
        0,
        (GLint)x,
        (GLint)y,
        (GLsizei)width,
        (GLsizei)height,
        upload_format,
        GL_UNSIGNED_BYTE,
        bytes);
    error = runtime->gl.GetError();
    if (error != GL_NO_ERROR) {
        set_diagnostic(
            diagnostic_out,
            error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                      : BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL texture upload failed");
        return error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                         : BOXRIG_GL_API_FAILURE;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "renderer texture rectangle was uploaded");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_draw_quads(
    boxrig_gl_runtime *runtime,
    uint32_t texture,
    const boxrig_gl_quad_instance *instances,
    size_t instance_count,
    boxrig_gl_blend blend,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLint sampler_location;
    GLenum error;
    size_t byte_count;

    if (runtime == NULL || !runtime->target_active || instances == NULL ||
        blend < BOXRIG_GL_BLEND_OVER || blend > BOXRIG_GL_BLEND_BEHIND ||
        instance_count == 0 || instance_count > INT_MAX ||
        instance_count > SIZE_MAX / sizeof(*instances)) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "an active target and nonempty bounded instance batch are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    byte_count = instance_count * sizeof(*instances);
    if (byte_count > (size_t)PTRDIFF_MAX) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "instance batch is too large for OpenGL");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    runtime->gl.UseProgram(runtime->program);
    runtime->gl.BindVertexArray(runtime->vertex_array);
    runtime->gl.BindBuffer(GL_ARRAY_BUFFER, runtime->buffers[0]);
    runtime->gl.BufferData(
        GL_ARRAY_BUFFER,
        (GLsizeiptr)byte_count,
        instances,
        GL_STREAM_DRAW);
#define INSTANCE_ATTRIBUTE(index, count, member)                               \
    do {                                                                        \
        runtime->gl.VertexAttribPointer(                                        \
            (index),                                                            \
            (count),                                                            \
            GL_FLOAT,                                                           \
            GL_FALSE,                                                           \
            (GLsizei)sizeof(boxrig_gl_quad_instance),                           \
            (const void *)offsetof(boxrig_gl_quad_instance, member));            \
        runtime->gl.EnableVertexAttribArray(index);                             \
        runtime->gl.VertexAttribDivisor(index, 1);                              \
    } while (0)
    INSTANCE_ATTRIBUTE(0, 4, destination);
    INSTANCE_ATTRIBUTE(1, 4, texture_coordinates);
    INSTANCE_ATTRIBUTE(2, 4, color);
    INSTANCE_ATTRIBUTE(3, 1, source_kind);
#undef INSTANCE_ATTRIBUTE
    runtime->gl.ActiveTexture(GL_TEXTURE0);
    runtime->gl.BindTexture(
        GL_TEXTURE_2D,
        texture == 0 ? runtime->texture : texture);
    runtime->gl.BindSampler(0, runtime->sampler);
    sampler_location = runtime->gl.GetUniformLocation(runtime->program, "atlas");
    if (sampler_location >= 0) {
        runtime->gl.Uniform1i(sampler_location, 0);
    }
    runtime->gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    if (blend == BOXRIG_GL_BLEND_BEHIND) {
        runtime->gl.BlendFuncSeparate(
            GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
    } else if (blend == BOXRIG_GL_BLEND_REPLACE) {
        runtime->gl.BlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    } else {
        runtime->gl.BlendFuncSeparate(
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA,
            GL_ONE,
            GL_ONE_MINUS_SRC_ALPHA);
    }
    runtime->gl.DrawArraysInstanced(
        GL_TRIANGLE_STRIP,
        0,
        4,
        (GLsizei)instance_count);
    error = runtime->gl.GetError();
    if (error != GL_NO_ERROR) {
        set_diagnostic(
            diagnostic_out,
            error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                      : BOXRIG_GL_API_FAILURE,
            error,
            0,
            "OpenGL instance batch failed");
        return error == GL_OUT_OF_MEMORY ? BOXRIG_GL_OUT_OF_MEMORY
                                         : BOXRIG_GL_API_FAILURE;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "renderer instance batch was queued");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_delete_texture(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    uint32_t *texture,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLuint name;

    if (runtime == NULL || texture == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime and texture output are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    name = *texture;
    *texture = 0;
    if (name != 0) {
        runtime->gl.DeleteTextures(1, &name);
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "renderer texture was deleted");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_insert_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLsync fence;

    if (runtime == NULL || fence_out == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime and fence output are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    memset(fence_out, 0, sizeof(*fence_out));
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    fence = runtime->gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_API_FAILURE,
            runtime->gl.GetError(),
            0,
            "OpenGL could not create a resource fence");
        return BOXRIG_GL_API_FAILURE;
    }
    fence_out->handle = fence;
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "resource fence was inserted");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_poll_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence,
    bool *signaled_out,
    boxrig_gl_diagnostic *diagnostic_out)
{
    GLenum result;

    if (runtime == NULL || fence == NULL || fence->handle == NULL ||
        signaled_out == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime, fence, and signal output are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *signaled_out = false;
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    result = runtime->gl.ClientWaitSync((GLsync)fence->handle, 0, 0);
    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
        *signaled_out = true;
    } else if (result != GL_TIMEOUT_EXPIRED) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_API_FAILURE,
            runtime->gl.GetError(),
            0,
            "OpenGL fence status query failed");
        return BOXRIG_GL_API_FAILURE;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        *signaled_out ? "resource fence is signaled"
                      : "resource fence is pending");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_delete_fence(
    boxrig_gl_runtime *runtime,
    uint64_t context_generation,
    boxrig_gl_fence *fence,
    boxrig_gl_diagnostic *diagnostic_out)
{
    if (runtime == NULL || fence == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime and fence are required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        return diagnostic_out != NULL ? diagnostic_out->status
                                      : BOXRIG_GL_CONTEXT_MISMATCH;
    }
    if (fence->handle != NULL) {
        runtime->gl.DeleteSync((GLsync)fence->handle);
        fence->handle = NULL;
    }
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "resource fence was deleted");
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_get_state_counters(
    const boxrig_gl_runtime *runtime,
    boxrig_gl_state_counters *counters_out)
{
    if (runtime == NULL || counters_out == NULL) {
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    *counters_out = runtime->counters;
    return BOXRIG_GL_OK;
}

boxrig_gl_status boxrig_gl_runtime_destroy(
    boxrig_gl_runtime **runtime_pointer,
    uint64_t context_generation,
    boxrig_gl_diagnostic *diagnostic_out)
{
    boxrig_gl_runtime *runtime;

    if (runtime_pointer == NULL || *runtime_pointer == NULL) {
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_INVALID_ARGUMENT,
            0,
            0,
            "runtime is required");
        return BOXRIG_GL_INVALID_ARGUMENT;
    }
    runtime = *runtime_pointer;
    *runtime_pointer = NULL;
    if (!current_context_matches(runtime, context_generation, diagnostic_out)) {
        free(runtime);
        return BOXRIG_GL_CONTEXT_MISMATCH;
    }
    if (runtime->target_active) {
        restore_state(runtime);
        runtime->target_active = false;
    }
    clear_errors(&runtime->gl);
    delete_resources(runtime);
    if (runtime->gl.GetError() != GL_NO_ERROR) {
        free(runtime);
        set_diagnostic(
            diagnostic_out,
            BOXRIG_GL_API_FAILURE,
            0,
            0,
            "OpenGL resource destruction failed");
        return BOXRIG_GL_API_FAILURE;
    }
    free(runtime);
    set_diagnostic(
        diagnostic_out,
        BOXRIG_GL_OK,
        0,
        0,
        "OpenGL runtime was destroyed");
    return BOXRIG_GL_OK;
}
