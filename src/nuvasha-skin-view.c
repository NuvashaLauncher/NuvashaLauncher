/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "nuvasha-skin-view.h"

#include <epoxy/gl.h>
#include <math.h>

typedef struct { float x, y, z, u, v; } Vertex;

struct _NuvashaSkinView
{
  GtkGLArea parent_instance;
  GdkTexture *skin;
  GdkTexture *cape;
  GLuint skin_texture;
  GLuint cape_texture;
  GLuint vertex_buffer;
  GLuint vertex_array;
  GLuint program;
  gboolean slim;
  gboolean slim_texture;
  gboolean ready;
  double yaw;
  double pitch;
  double drag_x;
  double drag_y;
};

G_DEFINE_FINAL_TYPE (NuvashaSkinView, nuvasha_skin_view, GTK_TYPE_GL_AREA)

static const char *vertex_shader =
  "#version 330 core\n"
  "in vec3 position; in vec2 texcoord; out vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader =
  "#version 330 core\n"
  "in vec2 uv; uniform sampler2D texture0; out vec4 color;\n"
  "void main() { color = texture(texture0, uv); if (color.a < 0.1) discard; }\n";

static const char *vertex_shader_legacy =
  "#version 120\n"
  "attribute vec3 position; attribute vec2 texcoord; varying vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader_legacy =
  "#version 120\n"
  "varying vec2 uv; uniform sampler2D texture0;\n"
  "void main() { gl_FragColor = texture2D(texture0, uv); if (gl_FragColor.a < 0.1) discard; }\n";

static const char *vertex_shader_es =
  "precision mediump float; attribute vec3 position; attribute vec2 texcoord; varying vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader_es =
  "precision mediump float; varying vec2 uv; uniform sampler2D texture0;\n"
  "void main() { gl_FragColor = texture2D(texture0, uv); if (gl_FragColor.a < 0.1) discard; }\n";

static GLuint
compile_shader (GLenum type, const char *source)
{
  GLint compiled = GL_FALSE;
  char log[1024] = { 0 };
  GLuint shader = glCreateShader (type);
  glShaderSource (shader, 1, &source, NULL);
  glCompileShader (shader);
  glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled)
    {
      glGetShaderInfoLog (shader, sizeof log - 1, NULL, log);
      g_warning ("Skin preview shader compilation failed: %s", log);
      glDeleteShader (shader);
      return 0;
    }
  return shader;
}

static void
upload_texture (NuvashaSkinView *self, GdkTexture *texture, GLuint *handle,
                gboolean skin)
{
  int width, height;
  gsize stride;
  g_autofree guchar *source = NULL;
  g_autofree guchar *pixels = NULL;
  g_autofree guchar *normalized = NULL;

  if (texture == NULL)
    {
      if (*handle != 0)
        glDeleteTextures (1, handle);
      *handle = 0;
      return;
    }

  width = gdk_texture_get_width (texture);
  height = gdk_texture_get_height (texture);
  stride = (gsize) width * 4;
  source = g_malloc (stride * height);
  gdk_texture_download (texture, source, stride);

  /* Slim atlases reserve transparent columns between the three-pixel arm
   * faces. If the user asks to display that atlas with classic four-pixel
   * geometry, those columns must not be sampled as part of the arm. */
  if (skin)
    {
      /* The last column of the classic right-arm back face is unused in a
       * standard slim atlas. Check it against an opaque arm pixel rather
       * than counting every gap: several valid Alex skins keep the other
       * reserved columns opaque. */
      self->slim_texture = height > 20 &&
        source[(gsize) 20 * stride + 55 * 4 + 3] < 128 &&
        source[(gsize) 20 * stride + 40 * 4 + 3] >= 128;
    }

  /* PrismLauncher’s legacy skin compatibility path expands a slim atlas to
   * the classic arm layout before drawing it. Without these copied columns,
   * the classic back-arm UVs land on the unused slim columns (usually black).
   * Keep the source image intact; this only changes the OpenGL preview. */
  if (skin && !self->slim && self->slim_texture && width == 64 && height == 64)
    {
      g_autofree guchar *expanded = g_memdup2 (source, stride * height);

      for (int y = 16; y < 32; y++)
        for (int x = 0; x < 9; x++)
          memcpy (expanded + (gsize) y * stride + (gsize) (46 + x) * 4,
                  source + (gsize) y * stride + (gsize) (45 + x) * 4, 4);
      for (int y = 16; y < 20; y++)
        for (int x = 0; x < 2; x++)
          memcpy (expanded + (gsize) y * stride + (gsize) (50 + x) * 4,
                  source + (gsize) y * stride + (gsize) (49 + x) * 4, 4);
      for (int y = 20; y < 32; y++)
        for (int x = 0; x < 2; x++)
          memcpy (expanded + (gsize) y * stride + (gsize) (54 + x) * 4,
                  source + (gsize) y * stride + (gsize) (53 + x) * 4, 4);

      /* The base atlas is made opaque below. Fill any remaining transparent
       * slim-only columns first, otherwise that normalization turns them
       * into black pixels on classic arms. */
      const int arm_ranges[][2] = { { 40, 56 }, { 32, 48 } };
      const int arm_rows[][2] = { { 16, 32 }, { 48, 64 } };
      for (guint range = 0; range < G_N_ELEMENTS (arm_ranges); range++)
        for (int y = arm_rows[range][0] + 4; y < arm_rows[range][1]; y++)
          for (int x = arm_ranges[range][0]; x < arm_ranges[range][1]; x++)
            {
              guchar *pixel = expanded + (gsize) y * stride + (gsize) x * 4;
              if (pixel[3] >= 128)
                continue;
              int source_x = -1;
              for (int distance = 1; distance < 16 && source_x < 0; distance++)
                {
                  int left = x - distance, right = x + distance;
                  if (left >= arm_ranges[range][0] &&
                      expanded[(gsize) y * stride + (gsize) left * 4 + 3] >= 128)
                    source_x = left;
                  else if (right < arm_ranges[range][1] &&
                           expanded[(gsize) y * stride + (gsize) right * 4 + 3] >= 128)
                    source_x = right;
                }
              if (source_x >= 0)
                memcpy (pixel, expanded + (gsize) y * stride +
                        (gsize) source_x * 4, 4);
            }
      g_free (source);
      source = g_steal_pointer (&expanded);
    }

  /* Expand legacy 64x32 skins to the modern 64x64 atlas, matching
   * PrismLauncher’s improveSkin() conversion. */
  if (skin && width == 64 && height == 32)
    {
      static const int legacy_faces[][6] = {
        { 4, 16, 16, 32, 4, 4 }, { 8, 16, 16, 32, 4, 4 },
        { 0, 20, 24, 32, 4, 12 }, { 4, 20, 16, 32, 4, 12 },
        { 8, 20, 8, 32, 4, 12 }, { 12, 20, 16, 32, 4, 12 },
        { 44, 16, -8, 32, 4, 4 }, { 48, 16, -8, 32, 4, 4 },
        { 40, 20, 0, 32, 4, 12 }, { 44, 20, -8, 32, 4, 12 },
        { 48, 20, -16, 32, 4, 12 }, { 52, 20, -8, 32, 4, 12 }
      };
      gsize old_stride = stride;

      normalized = g_malloc0 (64 * 64 * 4);
      for (int y = 0; y < 32; y++)
        memcpy (normalized + (gsize) y * 64 * 4,
                source + (gsize) y * old_stride, 64 * 4);

      for (guint i = 0; i < G_N_ELEMENTS (legacy_faces); i++)
        {
          int sx = legacy_faces[i][0], sy = legacy_faces[i][1];
          int dx = sx + legacy_faces[i][2], dy = sy + legacy_faces[i][3];
          int rw = legacy_faces[i][4], rh = legacy_faces[i][5];

          for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++)
              memcpy (normalized + (gsize) (dy + y) * 64 * 4 + (gsize) (dx + x) * 4,
                      source + (gsize) (sy + y) * old_stride +
                      (gsize) (sx + rw - 1 - x) * 4, 4);
        }

      g_free (source);
      source = g_steal_pointer (&normalized);
      width = height = 64;
      stride = 64 * 4;
    }

  /* Allocate this only after legacy skins have been expanded. */
  pixels = g_malloc (stride * height);

  /* PrismLauncher normalizes the alpha channel of skin files before giving
   * them to OpenGL.  A number of otherwise valid Minecraft skins have
   * transparent pixels in the base-layer atlas regions (often caused by
   * image editors or old skin converters).  Those regions are not optional
   * layers: they are the head, body and limbs and must be opaque. */
  if (width == 64 && (height == 64 || height == 32))
    {
      int opaque_height = MIN (height, 16);

      for (int y = 0; y < opaque_height; y++)
        for (int x = 0; x < MIN (width, 32); x++)
          source[(gsize) y * stride + (gsize) x * 4 + 3] = 255;

      for (int y = 16; y < MIN (height, 32); y++)
        for (int x = 0; x < width; x++)
          source[(gsize) y * stride + (gsize) x * 4 + 3] = 255;

      if (height == 64)
        for (int y = 48; y < 64; y++)
          for (int x = 16; x < 48; x++)
            source[(gsize) y * stride + (gsize) x * 4 + 3] = 255;
    }

  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        guchar *from = source + (gsize) y * stride + (gsize) x * 4;
        guchar *to = pixels + (gsize) y * stride + (gsize) x * 4;
        to[0] = from[2]; to[1] = from[1]; to[2] = from[0]; to[3] = from[3];
      }

  if (*handle == 0)
    glGenTextures (1, handle);
  glBindTexture (GL_TEXTURE_2D, *handle);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  glBindTexture (GL_TEXTURE_2D, 0);
}

static void
matrix_identity (float *m)
{
  memset (m, 0, sizeof (float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void
matrix_multiply (float *out, const float *a, const float *b)
{
  float result[16];
  for (int col = 0; col < 4; col++)
    for (int row = 0; row < 4; row++)
      result[col * 4 + row] =
        a[0 * 4 + row] * b[col * 4 + 0] +
        a[1 * 4 + row] * b[col * 4 + 1] +
        a[2 * 4 + row] * b[col * 4 + 2] +
        a[3 * 4 + row] * b[col * 4 + 3];
  memcpy (out, result, sizeof result);
}

static void
matrix_rotate (float *m, float radians, float x, float y, float z)
{
  float r[16];
  float c = cosf (radians), s = sinf (radians), one = 1.f - c;
  matrix_identity (r);
  r[0] = x * x * one + c; r[4] = x * y * one - z * s; r[8] = x * z * one + y * s;
  r[1] = y * x * one + z * s; r[5] = y * y * one + c; r[9] = y * z * one - x * s;
  r[2] = z * x * one - y * s; r[6] = z * y * one + x * s; r[10] = z * z * one + c;
  matrix_multiply (m, m, r);
}

static void
add_face (GArray *vertices, float x, float y, float z, float w, float h, float d,
          int face, int tx, int ty, int tw, int th, int texture_height)
{
  float x0 = x, x1 = x + w, y0 = y, y1 = y + h, z0 = z, z1 = z + d;
  float u0 = (float) tx / 64.f, u1 = (float) (tx + tw) / 64.f;
  /* GdkTexture downloads rows top-to-bottom and upload_texture keeps that
   * order.  PrismLauncher mirrors its QImage before uploading, so its UVs
   * use 1-v; nuvasha must address the unmirrored atlas directly instead. */
  float v0 = (float) (ty + th) / texture_height;
  float v1 = (float) ty / texture_height;
  Vertex q[4];
  /* PrismLauncher uses a triangle-strip order: bottom-left, bottom-right,
   * top-left, top-right.  Keep the indices consistent with that order. */
  switch (face)
    {
    case 0: q[0]=(Vertex){x0,y0,z1,u0,v0}; q[1]=(Vertex){x1,y0,z1,u1,v0}; q[2]=(Vertex){x0,y1,z1,u0,v1}; q[3]=(Vertex){x1,y1,z1,u1,v1}; break;
    case 1: q[0]=(Vertex){x1,y0,z1,u0,v0}; q[1]=(Vertex){x1,y0,z0,u1,v0}; q[2]=(Vertex){x1,y1,z1,u0,v1}; q[3]=(Vertex){x1,y1,z0,u1,v1}; break;
    case 2: q[0]=(Vertex){x1,y0,z0,u0,v0}; q[1]=(Vertex){x0,y0,z0,u1,v0}; q[2]=(Vertex){x1,y1,z0,u0,v1}; q[3]=(Vertex){x0,y1,z0,u1,v1}; break;
    case 3: q[0]=(Vertex){x0,y0,z0,u0,v0}; q[1]=(Vertex){x0,y0,z1,u1,v0}; q[2]=(Vertex){x0,y1,z0,u0,v1}; q[3]=(Vertex){x0,y1,z1,u1,v1}; break;
    case 4: q[0]=(Vertex){x0,y0,z0,u0,v1}; q[1]=(Vertex){x1,y0,z0,u1,v1}; q[2]=(Vertex){x0,y0,z1,u0,v0}; q[3]=(Vertex){x1,y0,z1,u1,v0}; break;
    default:q[0]=(Vertex){x0,y1,z1,u0,v0}; q[1]=(Vertex){x1,y1,z1,u1,v0}; q[2]=(Vertex){x0,y1,z0,u0,v1}; q[3]=(Vertex){x1,y1,z0,u1,v1}; break;
    }
  guint indices[] = { 0, 1, 2, 1, 2, 3 };
  for (int i = 0; i < 6; i++) g_array_append_val (vertices, q[indices[i]]);
}

static void
add_box (GArray *v, float x, float y, float z, float w, float h, float d,
         int tx, int ty, int texture_w, int texture_h, int texture_d,
         int texture_height)
{
  /* Minecraft's atlas lays a cuboid out as right/front/left/back on the
   * side strip, with top and bottom immediately above it. */
  add_face (v, x, y, z, w, h, d, 0, tx + texture_d, ty + texture_d,
            texture_w, texture_h, texture_height);
  add_face (v, x, y, z, w, h, d, 1, tx + texture_d + texture_w, ty + texture_d,
            texture_d, texture_h, texture_height);
  add_face (v, x, y, z, w, h, d, 2, tx + 2 * texture_d + texture_w, ty + texture_d,
            texture_w, texture_h, texture_height);
  add_face (v, x, y, z, w, h, d, 3, tx, ty + texture_d,
            texture_d, texture_h, texture_height);
  add_face (v, x, y, z, w, h, d, 4, tx + texture_d + texture_w, ty,
            texture_w, texture_d, texture_height);
  add_face (v, x, y, z, w, h, d, 5, tx + texture_d, ty,
            texture_w, texture_d, texture_height);
}

static void
rotate_cape (GArray *vertices)
{
  float radians = 10.8f * (float) G_PI / 180.f;
  float c = cosf (radians), s = sinf (radians);
  for (guint i = 0; i < vertices->len; i++)
    {
      Vertex *vertex = &g_array_index (vertices, Vertex, i);
      float y;

      /* PrismLauncher turns the cape around its Y axis first, putting it
       * behind the player, then lets it hang back slightly around X. */
      vertex->x = -vertex->x;
      vertex->z = -vertex->z;
      y = vertex->y;
      vertex->y = y * c - vertex->z * s;
      vertex->z = y * s + vertex->z * c;
    }
}

static void
draw_model (NuvashaSkinView *self)
{
  GArray *v = g_array_new (FALSE, FALSE, sizeof (Vertex));
  GArray *cape_v = g_array_new (FALSE, FALSE, sizeof (Vertex));
  int arm_width = self->slim ? 3 : 4;
  int arm_x = 40;
  float model[16], rotation[16], projection[16], mvp[16];
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  guint skin_count;
  float aspect = height > 0 ? (float) width / height : 1.f;

  if (self->skin == NULL || self->skin_texture == 0 || self->program == 0)
    { g_array_unref (v); g_array_unref (cape_v); return; }
  add_box (v, -4, 0, -4, 8, 8, 8, 0, 0, 8, 8, 8, 64);
  add_box (v, -4, -12, -2, 8, 12, 4, 16, 16, 8, 12, 4, 64);
  add_box (v, -3.9f, -24, -2, 4, 12, 4, 0, 16, 4, 12, 4, 64);
  add_box (v, -.1f, -24, -2, 4, 12, 4, 16, 48, 4, 12, 4, 64);
   add_box (v, -4 - arm_width, -12, -2, arm_width, 12, 4, arm_x, 16, arm_width, 12, 4, 64);
   add_box (v, 4, -12, -2, arm_width, 12, 4, 32, 48, arm_width, 12, 4, 64);
  /* The second layer is deliberately slightly larger: this is the hat,
   * jacket, sleeves and trousers layer of the Minecraft skin format. */
  add_box (v, -4.5f, -.5f, -4.5f, 9, 9, 9, 32, 0, 8, 8, 8, 64);
  add_box (v, -4.25f, -12.25f, -2.25f, 8.5f, 12.5f, 4.5f, 16, 32, 8, 12, 4, 64);
  add_box (v, -4.15f, -24.25f, -2.25f, 4.5f, 12.5f, 4.5f, 0, 32, 4, 12, 4, 64);
  add_box (v, -.35f, -24.25f, -2.25f, 4.5f, 12.5f, 4.5f, 0, 48, 4, 12, 4, 64);
  add_box (v, -6 - (arm_width + .5f) / 2.f, -12.25f, -2.25f,
            arm_width + .5f, 12.5f, 4.5f, 40, 32, arm_width, 12, 4, 64);
  add_box (v, 6 - (arm_width + .5f) / 2.f, -12.25f, -2.25f,
            arm_width + .5f, 12.5f, 4.5f, 48, 48, arm_width, 12, 4, 64);
  skin_count = v->len;
  if (self->cape_texture != 0)
    {
      add_box (cape_v, -5, -16, 2, 10, 16, 1, 0, 0, 10, 16, 1, 32);
      rotate_cape (cape_v);
    }

  matrix_identity (rotation);
  matrix_rotate (rotation, (float) self->yaw, 0, 1, 0);
  matrix_rotate (rotation, (float) self->pitch, 1, 0, 0);
  matrix_identity (model);
  model[0] = model[5] = model[10] = .04f;
  model[13] = .30f;
  matrix_multiply (model, rotation, model);
  matrix_identity (projection);
  projection[0] = 1.55f / aspect; projection[5] = 1.55f;
  matrix_multiply (mvp, projection, model);

  glUseProgram (self->program);
  glUniformMatrix4fv (glGetUniformLocation (self->program, "mvp"), 1, GL_FALSE, mvp);
  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, self->skin_texture);
  glUniform1i (glGetUniformLocation (self->program, "texture0"), 0);
  glBindBuffer (GL_ARRAY_BUFFER, self->vertex_buffer);
  if (self->vertex_array != 0)
    glBindVertexArray (self->vertex_array);
  glEnableVertexAttribArray (0); glEnableVertexAttribArray (1);
  glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (Vertex), (void *) 0);
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                         (void *) (3 * sizeof (float)));
  g_array_append_vals (v, cape_v->data, cape_v->len);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (v->len * sizeof (Vertex)),
                v->data, GL_DYNAMIC_DRAW);
  glDrawArrays (GL_TRIANGLES, 0, (GLsizei) skin_count);
  if (cape_v->len > 0 && self->cape_texture != 0)
    {
      glBindTexture (GL_TEXTURE_2D, self->cape_texture);
      glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (Vertex), (void *) 0);
      glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                             (void *) (3 * sizeof (float)));
      glDrawArrays (GL_TRIANGLES, (GLint) skin_count,
                    (GLsizei) cape_v->len);
    }
  glDisableVertexAttribArray (0); glDisableVertexAttribArray (1);
  glBindBuffer (GL_ARRAY_BUFFER, 0);
  if (self->vertex_array != 0)
    glBindVertexArray (0);
  glBindTexture (GL_TEXTURE_2D, 0); glUseProgram (0);
  g_array_unref (v);
  g_array_unref (cape_v);
}

static void
on_realize (GtkGLArea *area, gpointer user_data G_GNUC_UNUSED)
{
  NuvashaSkinView *self = nuvasha_SKIN_VIEW (area);
  const char *vs_source;
  const char *fs_source;
  GLint linked = GL_FALSE;
  char log[1024] = { 0 };
  int major = 0, minor = 0;
  gtk_gl_area_make_current (area);
  if (gtk_gl_area_get_error (area) != NULL) return;
  gdk_gl_context_get_version (gtk_gl_area_get_context (area), &major, &minor);
  if (gdk_gl_context_get_use_es (gtk_gl_area_get_context (area)))
    {
      vs_source = vertex_shader_es;
      fs_source = fragment_shader_es;
    }
  else if (major > 3 || (major == 3 && minor >= 3))
    {
      vs_source = vertex_shader;
      fs_source = fragment_shader;
    }
  else
    {
      vs_source = vertex_shader_legacy;
      fs_source = fragment_shader_legacy;
    }
  GLuint vs = compile_shader (GL_VERTEX_SHADER, vs_source);
  GLuint fs = compile_shader (GL_FRAGMENT_SHADER, fs_source);
  if (vs == 0 || fs == 0)
    return;
  self->program = glCreateProgram (); glAttachShader (self->program, vs); glAttachShader (self->program, fs);
  glBindAttribLocation (self->program, 0, "position"); glBindAttribLocation (self->program, 1, "texcoord");
  glLinkProgram (self->program);
  glGetProgramiv (self->program, GL_LINK_STATUS, &linked);
  if (!linked)
    {
      glGetProgramInfoLog (self->program, sizeof log - 1, NULL, log);
      g_warning ("Skin preview shader link failed: %s", log);
      glDeleteShader (vs); glDeleteShader (fs);
      glDeleteProgram (self->program); self->program = 0;
      return;
    }
  glDeleteShader (vs); glDeleteShader (fs);
  self->ready = TRUE;
  glGenBuffers (1, &self->vertex_buffer);
  if (major >= 3)
    glGenVertexArrays (1, &self->vertex_array);
  upload_texture (self, self->skin, &self->skin_texture, TRUE);
  upload_texture (self, self->cape, &self->cape_texture, FALSE);
}

static void
on_unrealize (GtkGLArea *area, gpointer user_data G_GNUC_UNUSED)
{
  NuvashaSkinView *self = nuvasha_SKIN_VIEW (area);
  gtk_gl_area_make_current (area);
  if (self->skin_texture) glDeleteTextures (1, &self->skin_texture);
  if (self->cape_texture) glDeleteTextures (1, &self->cape_texture);
  if (self->vertex_buffer) glDeleteBuffers (1, &self->vertex_buffer);
  if (self->vertex_array) glDeleteVertexArrays (1, &self->vertex_array);
  if (self->program) glDeleteProgram (self->program);
  self->skin_texture = self->cape_texture = self->program = self->vertex_buffer = self->vertex_array = 0;
  self->ready = FALSE;
}

static gboolean
on_render (GtkGLArea *area, GdkGLContext *context G_GNUC_UNUSED)
{
  NuvashaSkinView *self = nuvasha_SKIN_VIEW (area);
  glViewport (0, 0, gtk_widget_get_width (GTK_WIDGET (area)),
              gtk_widget_get_height (GTK_WIDGET (area)));
  glClearColor (0.f, 0.f, 0.f, 0.f);
  glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable (GL_DEPTH_TEST); glEnable (GL_BLEND); glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  if (self->ready && self->program != 0) draw_model (self);
  return TRUE;
}

static void
on_drag_begin (GtkGestureDrag *gesture G_GNUC_UNUSED, double start_x G_GNUC_UNUSED,
               double start_y G_GNUC_UNUSED, gpointer user_data)
{
  NuvashaSkinView *self = user_data;
  self->drag_x = self->drag_y = 0;
}

static void
on_drag_update (GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
  NuvashaSkinView *self = user_data;
  self->yaw += (offset_x - self->drag_x) * .01;
  self->pitch += (offset_y - self->drag_y) * .01;
  self->drag_x = offset_x;
  self->drag_y = offset_y;
  if (self->pitch > 1.2) self->pitch = 1.2;
  if (self->pitch < -1.2) self->pitch = -1.2;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
nuvasha_skin_view_dispose (GObject *object)
{
  NuvashaSkinView *self = nuvasha_SKIN_VIEW (object);
  g_clear_object (&self->skin); g_clear_object (&self->cape);
  G_OBJECT_CLASS (nuvasha_skin_view_parent_class)->dispose (object);
}

static void
nuvasha_skin_view_class_init (NuvashaSkinViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkGLAreaClass *gl_class = GTK_GL_AREA_CLASS (klass);
  object_class->dispose = nuvasha_skin_view_dispose;
  gl_class->render = on_render;
}

static void
nuvasha_skin_view_init (NuvashaSkinView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (self), TRUE);
  gtk_gl_area_set_auto_render (GTK_GL_AREA (self), TRUE);
  g_signal_connect (self, "realize", G_CALLBACK (on_realize), NULL);
  g_signal_connect (self, "unrealize", G_CALLBACK (on_unrealize), NULL);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  /* The atlas front is on the opposite side of the initial model view. */
  self->yaw = G_PI; self->pitch = 0;
}

NuvashaSkinView *
nuvasha_skin_view_new (void)
{
  return g_object_new (nuvasha_TYPE_SKIN_VIEW, NULL);
}

void
nuvasha_skin_view_set_skin (NuvashaSkinView *self, GdkTexture *texture)
{
  g_set_object (&self->skin, texture);
  if (self->ready) { gtk_gl_area_make_current (GTK_GL_AREA (self)); upload_texture (self, texture, &self->skin_texture, TRUE); }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
nuvasha_skin_view_set_cape (NuvashaSkinView *self, GdkTexture *texture)
{
  g_set_object (&self->cape, texture);
  if (self->ready) { gtk_gl_area_make_current (GTK_GL_AREA (self)); upload_texture (self, texture, &self->cape_texture, FALSE); }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
nuvasha_skin_view_set_slim (NuvashaSkinView *self, gboolean slim)
{
  self->slim = slim;
  if (self->ready)
    {
      gtk_gl_area_make_current (GTK_GL_AREA (self));
      upload_texture (self, self->skin, &self->skin_texture, TRUE);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
