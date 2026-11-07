/* j2dagent -- a minimal JVMTI agent for the jbox2d ledger (j2d/hsledger.py): for every nmethod HotSpot installs, its
 * code bytes and its inlining-aware pc -> (method, bci) map; for every stub/interpreter blob, its range. Replaces
 * -XX:+PrintAssembly, which disassembles on the compiler thread and so delays tier-up: on 2026-10-03 JDK 21 ran this
 * jar at 0.50 ms/step under PrintAssembly against 0.36 without, with 69% of the window outside any printed blob.
 *
 *   java -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints -agentpath:libj2dagent.so=<out file>[,lite] ...
 *
 * `,lite` (native/mcnative.sh, native/mccmp.py) writes no C or B records: only names, nmethod ranges and pc maps, which
 * is all an inlining-aware stack needs, at a fraction of the output a Minecraft run would otherwise write under the lock
 * every compiler thread takes here.
 *
 * Output (text, one record per line; every address hex, every time CLOCK_MONOTONIC ns, the clock perf -k 1 uses):
 *   N <id> <class sig>::<name><sig>        a method, interned
 *   B <id> <hex bytecodes>                  its bytecodes (GetBytecodes), for the opcode at a bci
 *   M <t> <addr> <size> <id>                an nmethod of method <id> loaded at time t
 *   C <hex code bytes>                      its code, copied at load
 *   P <pc> <id0>:<bci0>[,<id1>:<bci1>...]   a PcDesc: innermost frame first (DebugNonSafepoints makes these dense)
 *   U <t> <addr>                            an nmethod unloaded
 *   S <t> <addr> <size> <name>              DynamicCodeGenerated (interpreter, stubs, adapters)
 * jmethodIDs are interned by pointer; a pointer reused after class unloading would alias, which this bench never does. */
#include <jvmti.h>
#include <jvmticmlr.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static jvmtiEnv *jv;
static FILE *out;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

#define NSLOT (1 << 20)   /* methods interned, inlinees included: Minecraft passes 2^16 */
static jmethodID seen[NSLOT];
static int seen_id[NSLOT];
static int next_id = 1;
static int lite;

static unsigned long long now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void hex(const unsigned char *p, long n) {
	static const char d[] = "0123456789abcdef";
	for (long i = 0; i < n; i++) {
		fputc(d[p[i] >> 4], out);
		fputc(d[p[i] & 15], out);
	}
}

/* caller holds mu */
static int intern(jmethodID m) {
	unsigned long h = ((unsigned long)m >> 3) & (NSLOT - 1);
	for (int k = 0; k < NSLOT; k++, h = (h + 1) & (NSLOT - 1)) {
		if (seen[h] == m) return seen_id[h];
		if (!seen[h]) {
			int id = next_id++;
			seen[h] = m;
			seen_id[h] = id;
			jclass kl;
			char *cs = NULL, *mn = NULL, *ms = NULL;
			if ((*jv)->GetMethodDeclaringClass(jv, m, &kl) == JVMTI_ERROR_NONE)
				(*jv)->GetClassSignature(jv, kl, &cs, NULL);
			(*jv)->GetMethodName(jv, m, &mn, &ms, NULL);
			fprintf(out, "N %d %s::%s%s\n", id, cs ? cs : "?", mn ? mn : "?", ms ? ms : "");
			jint bn = 0;
			unsigned char *bc = NULL;
			if (!lite && (*jv)->GetBytecodes(jv, m, &bn, &bc) == JVMTI_ERROR_NONE && bc) {
				fprintf(out, "B %d ", id);
				hex(bc, bn);
				fputc('\n', out);
				(*jv)->Deallocate(jv, bc);
			}
			if (cs) (*jv)->Deallocate(jv, (unsigned char *)cs);
			if (mn) (*jv)->Deallocate(jv, (unsigned char *)mn);
			if (ms) (*jv)->Deallocate(jv, (unsigned char *)ms);
			return id;
		}
	}
	return 0;
}

static void JNICALL on_load(jvmtiEnv *e, jmethodID m, jint size, const void *addr, jint map_len,
                            const jvmtiAddrLocationMap *map, const void *info) {
	(void)e; (void)map_len; (void)map;
	unsigned long long t = now_ns();
	pthread_mutex_lock(&mu);
	int id = intern(m);
	/* intern every frame BEFORE the M/C/P lines: intern() prints N/B records, which must not land mid-line */
	for (const jvmtiCompiledMethodLoadRecordHeader *h = info; h; h = h->next) {
		if (h->kind != JVMTI_CMLR_INLINE_INFO) continue;
		const jvmtiCompiledMethodLoadInlineRecord *r = (const void *)h;
		for (int i = 0; i < r->numpcs; i++)
			for (int k = 0; k < r->pcinfo[i].numstackframes; k++)
				intern(r->pcinfo[i].methods[k]);
	}
	fprintf(out, "M %llu %lx %d %d\n", t, (unsigned long)addr, size, id);
	if (!lite) {
		fputs("C ", out);
		hex((const unsigned char *)addr, size);
		fputc('\n', out);
	}
	for (const jvmtiCompiledMethodLoadRecordHeader *h = info; h; h = h->next) {
		if (h->kind != JVMTI_CMLR_INLINE_INFO) continue;
		const jvmtiCompiledMethodLoadInlineRecord *r = (const void *)h;
		for (int i = 0; i < r->numpcs; i++) {
			PCStackInfo *p = &r->pcinfo[i];
			fprintf(out, "P %lx ", (unsigned long)p->pc);
			for (int k = 0; k < p->numstackframes; k++)
				fprintf(out, "%s%d:%d", k ? "," : "", intern(p->methods[k]), p->bcis[k]);
			fputc('\n', out);
		}
	}
	pthread_mutex_unlock(&mu);
}

static void JNICALL on_unload(jvmtiEnv *e, jmethodID m, const void *addr) {
	(void)e; (void)m;
	unsigned long long t = now_ns();
	pthread_mutex_lock(&mu);
	fprintf(out, "U %llu %lx\n", t, (unsigned long)addr);
	pthread_mutex_unlock(&mu);
}

static void JNICALL on_dyn(jvmtiEnv *e, const char *name, const void *addr, jint len) {
	(void)e;
	unsigned long long t = now_ns();
	pthread_mutex_lock(&mu);
	fprintf(out, "S %llu %lx %d %s\n", t, (unsigned long)addr, len, name);
	pthread_mutex_unlock(&mu);
}

static void JNICALL on_death(jvmtiEnv *e, JNIEnv *j) {
	(void)e; (void)j;
	pthread_mutex_lock(&mu);
	fflush(out);
	pthread_mutex_unlock(&mu);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *opts, void *reserved) {
	(void)reserved;
	if ((*vm)->GetEnv(vm, (void **)&jv, JVMTI_VERSION_1_0) != JNI_OK) return 1;
	char path[4096];
	snprintf(path, sizeof path, "%s", opts && *opts ? opts : "j2dagent.txt");
	char *comma = strrchr(path, ',');
	if (comma && !strcmp(comma, ",lite")) { *comma = 0; lite = 1; }
	out = fopen(path, "w");
	if (!out) return 1;
	setvbuf(out, NULL, _IOFBF, 1 << 22);
	jvmtiCapabilities c;
	memset(&c, 0, sizeof c);
	c.can_generate_compiled_method_load_events = 1;
	c.can_get_bytecodes = 1;
	if ((*jv)->AddCapabilities(jv, &c) != JVMTI_ERROR_NONE) return 1;
	jvmtiEventCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.CompiledMethodLoad = on_load;
	cb.CompiledMethodUnload = on_unload;
	cb.DynamicCodeGenerated = on_dyn;
	cb.VMDeath = on_death;
	(*jv)->SetEventCallbacks(jv, &cb, sizeof cb);
	(*jv)->SetEventNotificationMode(jv, JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
	(*jv)->SetEventNotificationMode(jv, JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_UNLOAD, NULL);
	(*jv)->SetEventNotificationMode(jv, JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL);
	(*jv)->SetEventNotificationMode(jv, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL);
	return 0;
}
