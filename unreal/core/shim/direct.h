#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int unlink(const char* path);
int mkdir(const char* path);
char* getcwd(char* buf, int max);
int chdir(const char* path);
#ifdef __cplusplus
}
#endif
