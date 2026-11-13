/* GLU-style helpers for Halcyon's 3D library (userland/lib/gl/glu.c):
 * projection and camera matrices, mipmap building and image scaling,
 * project / unproject, error strings, and a few quadrics. */
#ifndef HAL_GLU_H
#define HAL_GLU_H
#include "gl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GLU_VERSION 0x1BAA0
#define GLU_EXTENSIONS 0x1BAA1
#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top);
void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ, GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ);
void gluPickMatrix(GLdouble x, GLdouble y, GLdouble width, GLdouble height, const GLint viewport[4]);
GLint gluProject(GLdouble objX, GLdouble objY, GLdouble objZ, const GLdouble model[16], const GLdouble proj[16],
                 const GLint view[4], GLdouble *winX, GLdouble *winY, GLdouble *winZ);
GLint gluUnProject(GLdouble winX, GLdouble winY, GLdouble winZ, const GLdouble model[16], const GLdouble proj[16],
                   const GLint view[4], GLdouble *objX, GLdouble *objY, GLdouble *objZ);
/* scale an image (box filter when shrinking, bilinear when growing) */
GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn, const void *dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, GLvoid *dataOut);
/* every mipmap level of an image, scaled to powers of two first if needed */
GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data);
const GLubyte *gluErrorString(GLenum error);
const GLubyte *gluGetString(GLenum name);

/* quadrics: smooth normals and texture coordinates as GLU makes them */
typedef struct GLUquadric GLUquadric;
typedef GLUquadric GLUquadricObj;
#define GLU_SMOOTH 100000
#define GLU_FLAT 100001
#define GLU_NONE 100002
#define GLU_OUTSIDE 100020
#define GLU_INSIDE 100021
#define GLU_FILL 100012
#define GLU_LINE 100011
#define GLU_POINT 100010
GLUquadric *gluNewQuadric(void);
void gluDeleteQuadric(GLUquadric *q);
void gluQuadricNormals(GLUquadric *q, GLenum normals);
void gluQuadricTexture(GLUquadric *q, GLboolean texture);
void gluQuadricOrientation(GLUquadric *q, GLenum orientation);
void gluQuadricDrawStyle(GLUquadric *q, GLenum style);
void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks);
void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height, GLint slices, GLint stacks);
void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops);

#ifdef __cplusplus
}
#endif
#endif
