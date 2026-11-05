/* Minimal glib/config shim so wasm-encoder.c compiles and runs on the host, for the
 * relocatable-body byte-identity gate. Nothing here is used by the real build. */
#ifndef WJ_ENCTEST_SHIM_H
#define WJ_ENCTEST_SHIM_H
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t  guint8;
typedef uint16_t guint16;
typedef uint32_t guint32;
typedef size_t   gsize;
typedef uint64_t guint64;
typedef int32_t  gint32;
typedef int64_t  gint64;
typedef int      gboolean;
typedef void    *gpointer;
#define TRUE 1
#define FALSE 0
#define g_malloc(n)      malloc(n)
#define g_malloc0(n)     calloc(1,(n))
#define g_realloc(p,n)   realloc((p),(n))
#define g_free(p)        free(p)
#define g_new(t,n)       ((t*)malloc(sizeof(t)*(size_t)(n)))
#define g_new0(t,n)      ((t*)calloc((size_t)(n),sizeof(t)))
#define g_snprintf       snprintf
#define g_assert(c)      do { if (!(c)) { fprintf(stderr,"assert %s:%d: %s\n",__FILE__,__LINE__,#c); abort(); } } while (0)
#define g_assertf(c,...) do { if (!(c)) { fprintf(stderr,"assert %s:%d: ",__FILE__,__LINE__); fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); abort(); } } while (0)
#define g_error(...)     do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); abort(); } while (0)
#endif
