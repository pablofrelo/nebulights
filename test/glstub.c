/* GLES2 stubs — let the scene logic run without hardware. */
#include <GLES2/gl2.h>
#include <string.h>

static GLuint next_id = 1;

void glGenTextures(GLsizei n, GLuint *t){ for(GLsizei i=0;i<n;i++) t[i]=next_id++; }
void glGenFramebuffers(GLsizei n, GLuint *f){ for(GLsizei i=0;i<n;i++) f[i]=next_id++; }
void glGenBuffers(GLsizei n, GLuint *b){ for(GLsizei i=0;i<n;i++) b[i]=next_id++; }
void glDeleteTextures(GLsizei n, const GLuint *t){ (void)n;(void)t; }
void glDeleteFramebuffers(GLsizei n, const GLuint *f){ (void)n;(void)f; }
void glDeleteShader(GLuint s){ (void)s; }

GLuint glCreateShader(GLenum t){ (void)t; return next_id++; }
GLuint glCreateProgram(void){ return next_id++; }
void glShaderSource(GLuint s,GLsizei c,const GLchar*const*v,const GLint*l){(void)s;(void)c;(void)v;(void)l;}
void glCompileShader(GLuint s){(void)s;}
void glAttachShader(GLuint p,GLuint s){(void)p;(void)s;}
void glBindAttribLocation(GLuint p,GLuint i,const GLchar*n){(void)p;(void)i;(void)n;}
void glLinkProgram(GLuint p){(void)p;}
void glUseProgram(GLuint p){(void)p;}
void glGetShaderiv(GLuint s,GLenum p,GLint*v){(void)s;(void)p;*v=GL_TRUE;}
void glGetProgramiv(GLuint s,GLenum p,GLint*v){(void)s;(void)p;*v=GL_TRUE;}
void glGetShaderInfoLog(GLuint s,GLsizei b,GLsizei*l,GLchar*o){(void)s;(void)b;(void)l;o[0]=0;}
void glGetProgramInfoLog(GLuint s,GLsizei b,GLsizei*l,GLchar*o){(void)s;(void)b;(void)l;o[0]=0;}
GLint glGetUniformLocation(GLuint p,const GLchar*n){(void)p;(void)n;return 0;}

void glBindTexture(GLenum t,GLuint x){(void)t;(void)x;}
void glTexImage2D(GLenum a,GLint b,GLint c,GLsizei d,GLsizei e,GLint f,GLenum g,GLenum h,const void*i)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;}
void glTexParameteri(GLenum a,GLenum b,GLint c){(void)a;(void)b;(void)c;}
void glBindFramebuffer(GLenum a,GLuint b){(void)a;(void)b;}
void glFramebufferTexture2D(GLenum a,GLenum b,GLenum c,GLuint d,GLint e){(void)a;(void)b;(void)c;(void)d;(void)e;}
GLenum glCheckFramebufferStatus(GLenum t){(void)t;return GL_FRAMEBUFFER_COMPLETE;}
void glActiveTexture(GLenum t){(void)t;}

void glBindBuffer(GLenum t,GLuint b){(void)t;(void)b;}
/* the key part: read the whole declared range, so ASan catches overruns */
void glBufferData(GLenum t,GLsizeiptr size,const void*data,GLenum u){
	(void)t;(void)u;
	if(data && size>0){ volatile char acc=0; const char*p=data;
		for(GLsizeiptr i=0;i<size;i++) acc^=p[i]; (void)acc; }
}
void glEnableVertexAttribArray(GLuint i){(void)i;}
void glDisableVertexAttribArray(GLuint i){(void)i;}
void glVertexAttribPointer(GLuint a,GLint b,GLenum c,GLboolean d,GLsizei e,const void*f)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void glDrawArrays(GLenum m,GLint f,GLsizei c){(void)m;(void)f;(void)c;}

void glUniform1i(GLint l,GLint v){(void)l;(void)v;}
void glUniform1f(GLint l,GLfloat v){(void)l;(void)v;}
void glUniform2f(GLint l,GLfloat a,GLfloat b){(void)l;(void)a;(void)b;}
void glUniformMatrix4fv(GLint l,GLsizei c,GLboolean t,const GLfloat*v){
	(void)l;(void)t;
	if(v){ volatile float a=0; for(int i=0;i<16*c;i++) a+=v[i]; (void)a; }
}
void glViewport(GLint a,GLint b,GLsizei c,GLsizei d){(void)a;(void)b;(void)c;(void)d;}
void glClearColor(GLfloat a,GLfloat b,GLfloat c,GLfloat d){(void)a;(void)b;(void)c;(void)d;}
void glClear(GLbitfield m){(void)m;}
void glEnable(GLenum c){(void)c;}
void glDisable(GLenum c){(void)c;}
void glBlendFunc(GLenum a,GLenum b){(void)a;(void)b;}
const GLubyte *glGetString(GLenum n){
	(void)n;
	return (const GLubyte*)"GL_OES_texture_half_float GL_OES_texture_half_float_linear";
}
