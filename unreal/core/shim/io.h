#pragma once
struct _finddata_t { char name[260]; };
#ifdef __cplusplus
extern "C" {
#endif
long _findfirst(const char* spec, struct _finddata_t* out);
int _findnext(long h, struct _finddata_t* out);
#ifdef __cplusplus
}
#endif
