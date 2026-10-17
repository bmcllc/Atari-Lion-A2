#pragma once
// Level geometry as the viewer draws it.
enum { LV_MAX_POLYS = 2048 };
struct LvPoly {
  unsigned nv;
  float v[16 * 3];
  float base[3], normal[3], tex_u[3], tex_v[3];
};
struct LvLevel {
  LvPoly polys[LV_MAX_POLYS];
  unsigned count;
  float start[3];
  float yaw;
  int have_start;
  const char* source;
};
bool lion_level_load(const char* file, LvLevel* lv);
