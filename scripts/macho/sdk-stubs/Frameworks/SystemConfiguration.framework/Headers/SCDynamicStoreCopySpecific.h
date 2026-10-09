/*
 * Stand-in for Apple's <SystemConfiguration/SCDynamicStoreCopySpecific.h>
 * for zig cc builds, which have no macOS SDK. It declares only the public API
 * that curl's lib/macos.c calls; the types are Apple's opaque CF pointers,
 * so the calls bind to the real framework at run time. See ../../../README.md.
 */
#ifndef ANYFS_SDK_STUB_SCDYNAMICSTORECOPYSPECIFIC_H
#define ANYFS_SDK_STUB_SCDYNAMICSTORECOPYSPECIFIC_H

typedef const void *CFTypeRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __SCDynamicStore *SCDynamicStoreRef;

void CFRelease(CFTypeRef cf);
CFDictionaryRef SCDynamicStoreCopyProxies(SCDynamicStoreRef store);

#endif
