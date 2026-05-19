#ifndef __PINVOKE_H__
#define __PINVOKE_H__

#include <stdint.h>

typedef struct {
	const char *name;
	void *func;
} PinvokeImport;

typedef struct {
	const char *name;
	PinvokeImport *imports;
	int count;
} PinvokeTable;

typedef struct {
	const char *key;
	uint32_t token;
	void *func;
} UnmanagedExport;

typedef struct {
	void *func;
	void *arg;
} InterpFtnDesc;

/*
 * Sentinel value stored in InterpFtnDesc.arg when .func points directly to an
 * AOT-compiled native-to-managed wrapper body, callable with the native
 * signature instead of the gsharedvt-style (int*, int*, ...) interp entry
 * convention. The generated wasm_native_to_interp_* stubs use this to decide
 * which calling path to take.
 */
#define WASM_N2M_AOT_DIRECT_ARG ((void*)(uintptr_t)-1)

void*
wasm_dl_lookup_pinvoke_table (const char *name);

int
wasm_dl_is_pinvoke_table (void *handle);

void*
wasm_dl_get_native_to_interp (uint32_t token, const char *key, void *extra_arg);

void
mono_wasm_pinvoke_vararg_stub (void);

typedef void* (*MonoWasmNativeToInterpCallback) (char * cookie);

void
mono_wasm_install_interp_to_native_callback (MonoWasmNativeToInterpCallback cb);

typedef struct _MonoInterpMethodArguments MonoInterpMethodArguments;

int 
mono_wasm_interp_method_args_get_iarg (MonoInterpMethodArguments *margs, int i);

int64_t
mono_wasm_interp_method_args_get_larg (MonoInterpMethodArguments *margs, int i);

float
mono_wasm_interp_method_args_get_farg (MonoInterpMethodArguments *margs, int i);

double
mono_wasm_interp_method_args_get_darg (MonoInterpMethodArguments *margs, int i);

void*
mono_wasm_interp_method_args_get_retval (MonoInterpMethodArguments *margs);

#endif
