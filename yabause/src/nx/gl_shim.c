/*
This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Client-side vertex array emulation for the desktop GL core profile.
//
// The renderer was written for GLES3, where glVertexAttribPointer may take a
// plain CPU pointer while vertex array object 0 is bound. A core profile
// context has no usable VAO 0 and rejects CPU pointers, so every draw fails.
// ygl.h routes the renderer's vertex calls here: while the renderer believes
// the default VAO is bound, ours stands in for it, CPU pointers are recorded,
// and they are copied into a streaming buffer object when the draw happens.
// Code that binds its own VAO (nanovg) is passed straight through.
//
// All of this state belongs to the single GL context, which only one thread
// holds at a time, so no locking is needed.

#define NX_GL_SHIM_IMPL
#include "ygl.h"

#include <stdio.h>
#include <string.h>

#define MAX_ATTRIBS 16
#define STREAM_BUFFER_SIZE (4 * 1024 * 1024)

typedef struct {
  int client;   // pointer refers to CPU memory, not a buffer object
  GLint size;
  GLenum type;
  GLboolean normalized;
  GLsizei stride;
  const void * pointer;
} ClientAttrib;

static ClientAttrib attribs[MAX_ATTRIBS];
static unsigned enabled_mask;

static GLuint vao;
static GLuint app_vao;        // VAO the renderer bound; 0 means the default, i.e. ours
static int vao_bound;         // ours is currently bound
static GLuint array_buffer;   // GL_ARRAY_BUFFER binding as the renderer sees it
static GLuint stream_vbo;
static GLsizeiptr stream_capacity = 0;  // no storage yet: first reserve allocates
static GLsizeiptr stream_offset = 0;

// Returns non-zero when the default VAO is in effect, so emulation applies.
static int BindShimVao(void)
{
  if (app_vao != 0) return 0;
  if (vao == 0) {
    glad_glGenVertexArrays(1, &vao);
    glad_glGenBuffers(1, &stream_vbo);
  }
  if (!vao_bound) {
    glad_glBindVertexArray(vao);
    vao_bound = 1;
  }
  return 1;
}

static GLsizei TypeSize(GLenum type)
{
  switch (type) {
  case GL_BYTE:
  case GL_UNSIGNED_BYTE:
    return 1;
  case GL_SHORT:
  case GL_UNSIGNED_SHORT:
  case GL_HALF_FLOAT:
    return 2;
  default:
    return 4;
  }
}

#define STREAM_ALIGN(x) (((x) + 15) & ~(GLsizeiptr)15)

// Makes room for 'total' bytes in the bound stream buffer. Must cover every
// attribute of a draw at once: orphaning replaces the storage of the whole
// buffer object, so attributes already uploaded for this draw would then
// point at discarded data.
static void StreamReserve(GLsizeiptr total)
{
  if (stream_offset + total <= stream_capacity) return;

  // Orphan: the driver hands out fresh storage, so draws still in flight
  // keep reading the old contents
  stream_capacity = total > STREAM_BUFFER_SIZE ? total : STREAM_BUFFER_SIZE;
  glad_glBufferData(GL_ARRAY_BUFFER, stream_capacity, NULL, GL_STREAM_DRAW);
  stream_offset = 0;
}

// Copies [data, data + size) into the bound stream buffer, which StreamReserve
// has made room in; returns its offset.
static GLintptr StreamUpload(const void * data, GLsizeiptr size)
{
  GLintptr offset;
  void * dst;

  offset = stream_offset;
  dst = glad_glMapBufferRange(GL_ARRAY_BUFFER, offset, size,
    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  if (dst == NULL) {
    glad_glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
  } else {
    memcpy(dst, data, size);
    glad_glUnmapBuffer(GL_ARRAY_BUFFER);
  }

  stream_offset = STREAM_ALIGN(offset + size);
  return offset;
}

void nx_glShimReset(void)
{
  // The objects belonged to the previous context, which is gone
  memset(attribs, 0, sizeof(attribs));
  enabled_mask = 0;
  vao = 0;
  app_vao = 0;
  vao_bound = 0;
  array_buffer = 0;
  stream_vbo = 0;
  stream_capacity = 0;
  stream_offset = 0;
}

void nx_glBindBuffer(GLenum target, GLuint buffer)
{
  if (target == GL_ARRAY_BUFFER) array_buffer = buffer;
  glad_glBindBuffer(target, buffer);
}

void nx_glBindVertexArray(GLuint array)
{
  app_vao = array;
  if (array == 0) {
    BindShimVao();
  } else {
    glad_glBindVertexArray(array);
    vao_bound = 0;
  }
}

void nx_glEnableVertexAttribArray(GLuint index)
{
  if (BindShimVao() && index < MAX_ATTRIBS) enabled_mask |= 1u << index;
  glad_glEnableVertexAttribArray(index);
}

void nx_glDisableVertexAttribArray(GLuint index)
{
  if (BindShimVao() && index < MAX_ATTRIBS) enabled_mask &= ~(1u << index);
  glad_glDisableVertexAttribArray(index);
}

void nx_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void * pointer)
{
  if (!BindShimVao() || index >= MAX_ATTRIBS) {
    glad_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    return;
  }

  attribs[index].client = (array_buffer == 0);
  attribs[index].size = size;
  attribs[index].type = type;
  attribs[index].normalized = normalized;
  attribs[index].stride = stride;
  attribs[index].pointer = pointer;

  // Buffer-backed attributes go straight to the driver; CPU ones wait for the draw
  if (!attribs[index].client)
    glad_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void nx_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
  unsigned i;
  GLsizeiptr bytes[MAX_ATTRIBS];
  GLsizeiptr total = 0;

  if (!BindShimVao()) {
    glad_glDrawArrays(mode, first, count);
    return;
  }
  if (count <= 0) return;

  // Size every CPU-side attribute first so the whole draw fits in one piece
  // of buffer storage (see StreamReserve)
  for (i = 0; i < MAX_ATTRIBS; i++) {
    ClientAttrib * a = &attribs[i];
    GLsizei elem, stride;

    bytes[i] = 0;
    if (!(enabled_mask & (1u << i)) || !a->client || a->pointer == NULL) continue;

    // Upload from the start of the array so 'first' still indexes correctly
    elem = a->size * TypeSize(a->type);
    stride = a->stride ? a->stride : elem;
    bytes[i] = (GLsizeiptr)(first + count - 1) * stride + elem;
    total += STREAM_ALIGN(bytes[i]);
  }

  if (total > 0) {
    glad_glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
    StreamReserve(total);
    for (i = 0; i < MAX_ATTRIBS; i++) {
      ClientAttrib * a = &attribs[i];
      GLintptr offset;
      if (bytes[i] == 0) continue;
      offset = StreamUpload(a->pointer, bytes[i]);
      glad_glVertexAttribPointer(i, a->size, a->type, a->normalized, a->stride, (const void *)offset);
    }
    glad_glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
  }

  glad_glDrawArrays(mode, first, count);
}
