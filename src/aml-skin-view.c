/* Copyright (C) 2026 Flia
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "aml-skin-view.h"

#include <epoxy/gl.h>
#include <math.h>

typedef struct { float x, y, z, u, v; } Vertex;

struct _AmlSkinView
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
  gboolean ready;
  double yaw;
  double pitch;
  double drag_x;
  double drag_y;
};

G_DEFINE_FINAL_TYPE (AmlSkinView, aml_skin_view, GTK_TYPE_GL_AREA)

static const char *vertex_shader =
  "#version 330 core\n"
  "in vec3 position; in vec2 texcoord; out vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader =
  "#version 330 core\n"
  "in vec2 uv; uniform sampler2D texture0; out vec4 color;\n"
  "void main() { color = texture(texture0, uv); }\n";

static const char *vertex_shader_legacy =
  "#version 120\n"
  "attribute vec3 position; attribute vec2 texcoord; varying vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader_legacy =
  "#version 120\n"
  "varying vec2 uv; uniform sampler2D texture0;\n"
  "void main() { gl_FragColor = texture2D(texture0, uv); }\n";

static const char *vertex_shader_es =
  "precision mediump float; attribute vec3 position; attribute vec2 texcoord; varying vec2 uv;\n"
  "uniform mat4 mvp;\n"
  "void main() { uv = texcoord; gl_Position = mvp * vec4(position, 1.0); }\n";

static const char *fragment_shader_es =
  "precision mediump float; varying vec2 uv; uniform sampler2D texture0;\n"
  "void main() { gl_FragColor = texture2D(texture0, uv); }\n";

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
upload_texture (GdkTexture *texture, GLuint *handle)
{
  int width, height;
  gsize stride;
  g_autofree guchar *source = NULL;
  g_autofree guchar *pixels = NULL;

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
  pixels = g_malloc (stride * height);
  gdk_texture_download (texture, source, stride);
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
  float v0 = 1.f - (float) (ty + th) / texture_height;
  float v1 = 1.f - (float) ty / texture_height;
  Vertex q[4];
  switch (face)
    {
    case 0: q[0]=(Vertex){x0,y0,z1,u0,v0}; q[1]=(Vertex){x1,y0,z1,u1,v0}; q[2]=(Vertex){x1,y1,z1,u1,v1}; q[3]=(Vertex){x0,y1,z1,u0,v1}; break;
    case 1: q[0]=(Vertex){x1,y0,z0,u0,v0}; q[1]=(Vertex){x0,y0,z0,u1,v0}; q[2]=(Vertex){x0,y1,z0,u1,v1}; q[3]=(Vertex){x1,y1,z0,u0,v1}; break;
    case 2: q[0]=(Vertex){x0,y0,z0,u0,v0}; q[1]=(Vertex){x0,y0,z1,u1,v0}; q[2]=(Vertex){x0,y1,z1,u1,v1}; q[3]=(Vertex){x0,y1,z0,u0,v1}; break;
    case 3: q[0]=(Vertex){x1,y0,z1,u0,v0}; q[1]=(Vertex){x1,y0,z0,u1,v0}; q[2]=(Vertex){x1,y1,z0,u1,v1}; q[3]=(Vertex){x1,y1,z1,u0,v1}; break;
    case 4: q[0]=(Vertex){x0,y1,z1,u0,v0}; q[1]=(Vertex){x1,y1,z1,u1,v0}; q[2]=(Vertex){x1,y1,z0,u1,v1}; q[3]=(Vertex){x0,y1,z0,u0,v1}; break;
    default:q[0]=(Vertex){x0,y0,z0,u0,v0}; q[1]=(Vertex){x1,y0,z0,u1,v0}; q[2]=(Vertex){x1,y0,z1,u1,v1}; q[3]=(Vertex){x0,y0,z1,u0,v1}; break;
    }
  guint indices[] = { 0, 1, 2, 0, 2, 3 };
  for (int i = 0; i < 6; i++) g_array_append_val (vertices, q[indices[i]]);
}

static void
add_box (GArray *v, float x, float y, float z, float w, float h, float d,
         int tx, int ty, int texture_height)
{
  /* Minecraft's atlas lays a cuboid out as right/front/left/back on the
   * side strip, with top and bottom immediately above it. */
  add_face (v, x, y, z, w, h, d, 0, tx + (int)d, ty + (int)d,
            (int)w, (int)h, texture_height);
  add_face (v, x, y, z, w, h, d, 1, tx + 2 * (int)d + (int)w, ty + (int)d,
            (int)w, (int)h, texture_height);
  add_face (v, x, y, z, w, h, d, 2, tx, ty + (int)d,
            (int)d, (int)h, texture_height);
  add_face (v, x, y, z, w, h, d, 3, tx + (int)d + (int)w, ty + (int)d,
            (int)d, (int)h, texture_height);
  add_face (v, x, y, z, w, h, d, 4, tx + (int)d, ty,
            (int)w, (int)d, texture_height);
  add_face (v, x, y, z, w, h, d, 5, tx + (int)d + (int)w, ty,
            (int)w, (int)d, texture_height);
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
draw_model (AmlSkinView *self)
{
  GArray *v = g_array_new (FALSE, FALSE, sizeof (Vertex));
  GArray *cape_v = g_array_new (FALSE, FALSE, sizeof (Vertex));
  int arm_width = self->slim ? 3 : 4;
  int arm_x = self->slim ? 40 : 40;
  int texture_height = 64;
  float model[16], rotation[16], projection[16], mvp[16];
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  guint skin_count;
  float aspect = height > 0 ? (float) width / height : 1.f;

  if (self->skin == NULL || self->skin_texture == 0 || self->program == 0)
    { g_array_unref (v); g_array_unref (cape_v); return; }
  if (gdk_texture_get_height (self->skin) == 32) texture_height = 32;
  add_box (v, -4, 0, -4, 8, 8, 8, 0, 0, texture_height);
  add_box (v, -4, -12, -2, 8, 12, 4, 16, 16, texture_height);
  add_box (v, -3.9f, -24, -2, 4, 12, 4, 0, 16, texture_height);
  add_box (v, -.1f, -24, -2, 4, 12, 4, 16, 48, texture_height);
  add_box (v, -4 - arm_width, -12, -2, arm_width, 12, 4, arm_x, 16, texture_height);
  add_box (v, 4, -12, -2, arm_width, 12, 4, 32, 48, texture_height);
  if (texture_height == 64)
    {
      /* The second layer is deliberately slightly larger: this is the hat,
       * jacket, sleeves and trousers layer of the Minecraft skin format. */
      add_box (v, -4.5f, -.5f, -4.5f, 9, 9, 9, 32, 0, texture_height);
      add_box (v, -4.25f, -12.25f, -2.25f, 8.5f, 12.5f, 4.5f, 16, 32, texture_height);
      add_box (v, -6 - (arm_width + .5f) / 2.f, -12.25f, -2.25f,
               arm_width + .5f, 12.5f, 4.5f, 40, 32, texture_height);
      add_box (v, 6 - (arm_width + .5f) / 2.f, -12.25f, -2.25f,
               arm_width + .5f, 12.5f, 4.5f, 48, 48, texture_height);
    }
  skin_count = v->len;
  if (self->cape_texture != 0)
    {
      add_box (cape_v, -5, -16, 2, 10, 16, 1, 0, 0, 32);
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
  g_array_append_vals (v, cape_v->data, cape_v->len);
  glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (v->len * sizeof (Vertex)),
                v->data, GL_DYNAMIC_DRAW);
  glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof (Vertex), (void *) 0);
  glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof (Vertex),
                         (void *) (3 * sizeof (float)));
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
  AmlSkinView *self = AML_SKIN_VIEW (area);
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
  upload_texture (self->skin, &self->skin_texture);
  upload_texture (self->cape, &self->cape_texture);
}

static void
on_unrealize (GtkGLArea *area, gpointer user_data G_GNUC_UNUSED)
{
  AmlSkinView *self = AML_SKIN_VIEW (area);
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
  AmlSkinView *self = AML_SKIN_VIEW (area);
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
  AmlSkinView *self = user_data;
  self->drag_x = self->drag_y = 0;
}

static void
on_drag_update (GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
  AmlSkinView *self = user_data;
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
aml_skin_view_dispose (GObject *object)
{
  AmlSkinView *self = AML_SKIN_VIEW (object);
  g_clear_object (&self->skin); g_clear_object (&self->cape);
  G_OBJECT_CLASS (aml_skin_view_parent_class)->dispose (object);
}

static void
aml_skin_view_class_init (AmlSkinViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkGLAreaClass *gl_class = GTK_GL_AREA_CLASS (klass);
  object_class->dispose = aml_skin_view_dispose;
  gl_class->render = on_render;
}

static void
aml_skin_view_init (AmlSkinView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (self), TRUE);
  gtk_gl_area_set_auto_render (GTK_GL_AREA (self), TRUE);
  g_signal_connect (self, "realize", G_CALLBACK (on_realize), NULL);
  g_signal_connect (self, "unrealize", G_CALLBACK (on_unrealize), NULL);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  self->yaw = 0; self->pitch = 0;
}

AmlSkinView *
aml_skin_view_new (void)
{
  return g_object_new (AML_TYPE_SKIN_VIEW, NULL);
}

void
aml_skin_view_set_skin (AmlSkinView *self, GdkTexture *texture)
{
  g_set_object (&self->skin, texture);
  if (self->ready) { gtk_gl_area_make_current (GTK_GL_AREA (self)); upload_texture (texture, &self->skin_texture); }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
aml_skin_view_set_cape (AmlSkinView *self, GdkTexture *texture)
{
  g_set_object (&self->cape, texture);
  if (self->ready) { gtk_gl_area_make_current (GTK_GL_AREA (self)); upload_texture (texture, &self->cape_texture); }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
aml_skin_view_set_slim (AmlSkinView *self, gboolean slim)
{
  self->slim = slim; gtk_widget_queue_draw (GTK_WIDGET (self));
}
