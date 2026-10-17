#pragma once
typedef long time_t;
typedef long clock_t;
#ifdef __cplusplus
extern "C" {
#endif
time_t time(time_t* t);
clock_t clock(void);
#ifdef __cplusplus
}
#endif
