// Helpers for host-implemented COM objects (see com.c).
#pragma once
#include "win32.h"

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } Guid;
extern const Guid IID_IUnknown_;

u64 *rt_com_vtable(const char *cls, const char *const *methods);   // thunks for "__wintomac_<cls>_<method>"
int rt_guid_eq(u64 guest_guid_ptr, const Guid *want);
void rt_guid_str(u64 guid, char *out, size_t n);
void rt_com_register(const Guid *clsid, u32 (*create)(Ctx *c, u64 iid, u64 out), const char *name);

#define S_OK_ 0u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_INVALIDARG_ 0x80070057u
#define E_NOTIMPL_ 0x80004001u
#define E_FAIL__ 0x80004005u

// Standard IUnknown for objects whose first two fields are { u64 vtbl; u32 refs; }
typedef struct { u64 vtbl; u32 refs; } ComHead;
#define COM_ADDREF(obj) __atomic_add_fetch(&((ComHead *)(obj))->refs, 1, __ATOMIC_SEQ_CST)
#define COM_RELEASE(obj) __atomic_sub_fetch(&((ComHead *)(obj))->refs, 1, __ATOMIC_SEQ_CST)
