// wasm setjmp/longjmp for the FreeType objects (built with -sSUPPORT_LONGJMP=wasm) in a build linked with
// -sSUPPORT_LONGJMP=0, whose compiler-rt has __wasm_setjmp but not these two: the same code as Emscripten's
// system/lib/compiler-rt/emscripten_setjmp.c (__WASM_SJLJ__ part) and __c_longjmp.S. MIT / UIUC (Emscripten).
struct jmp_buf_impl { void* func_invocation_id; unsigned label; struct { void* env; int val; } arg; };

__asm__(".globl __c_longjmp\n.tagtype __c_longjmp i32\n__c_longjmp:\n");

void __wasm_longjmp(void* env, int val) {
    struct jmp_buf_impl* jb = env;
    jb->arg.env = env;
    jb->arg.val = val ? val : 1;
    __builtin_wasm_throw(1, &jb->arg); // tag 1 is __c_longjmp
}
