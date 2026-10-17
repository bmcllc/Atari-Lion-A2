// Loads an Unreal level on the Lion with Unreal's own Core: ULinkerLoad opens the package
// (summary, names, imports, exports) and its archive operators deserialize the BSP.
#include "CorePrivate.h"
#include "UnLinker.h"
#include "level.h"

namespace {

ULinkerLoad* L;

INT Index() { INT v = 0; *L << AR_INDEX(v); return v; }
FLOAT Float() { FLOAT f = 0; *L << f; return f; }
DWORD Dword() { DWORD d = 0; *L << d; return d; }
BYTE Byte() { BYTE b = 0; *L << b; return b; }

// Skips the tagged property list at the current position; picks out Location and Rotation.
struct Props {
  float location[3];
  INT yaw;
  UBOOL has_location;
};
void ReadProps(Props* out) {
  for (;;) {
    FName Name;
    ((FArchive&)*L) << Name;
    if (Name == NAME_None) break;
    const BYTE Info = Byte();
    if ((Info & 15) == NAME_StructProperty) { FName Struct; ((FArchive&)*L) << Struct; }
    INT Size;
    switch (Info & 0x70) {
      case 0x00: Size = 1; break;
      case 0x10: Size = 2; break;
      case 0x20: Size = 4; break;
      case 0x30: Size = 12; break;
      case 0x40: Size = 16; break;
      case 0x50: Size = Byte(); break;
      case 0x60: Size = Byte(); Size |= Byte() << 8; break;
      default: Size = (INT)Dword(); break;
    }
    if ((Info & 0x80) && (Info & 15) != NAME_BoolProperty) {
      const BYTE B = Byte();
      if (B & 0x80) Byte();
      if ((B & 0xC0) == 0xC0) { Byte(); Byte(); }
    }
    const INT End = L->Tell() + Size;
    if (out && Size == 12 && Name == FName("Location", FNAME_Find)) {
      for (INT i = 0; i < 3; ++i) out->location[i] = Float();
      out->has_location = 1;
    } else if (out && Size == 12 && Name == FName("Rotation", FNAME_Find)) {
      Dword();
      out->yaw = (INT)Dword();
    }
    L->Seek(End);
  }
}

// Skips what UObject::Serialize writes before the properties of an object with an execution stack.
void SkipStack(const FObjectExport& E) {
  if (!(E.ObjectFlags & RF_HasStack)) return;
  const INT Node = Index();
  Index();
  for (INT i = 0; i < 12; ++i) Byte();
  if (Node) Index();
}

void BeginExport(const FObjectExport& E, Props* props = 0) {
  L->Seek(E.SerialOffset);
  SkipStack(E);
  ReadProps(props);
}

// UDatabase header after the properties: element count and capacity. Returns the count.
INT BeginDatabase(INT Obj) {
  if (Obj < 1 || Obj > L->ExportMap.Num()) return -1;
  BeginExport(L->ExportMap(Obj - 1));
  const INT Count = (INT)Dword();
  Dword();
  return Count > 0 && Count < 100000 ? Count : (Count == 0 ? 0 : -1);
}

float* ReadVectors(INT Obj, INT* Count) {
  *Count = BeginDatabase(Obj);
  if (*Count < 0) return 0;
  float* v = (float*)appMalloc(*Count * 12 + 4, "vec");
  for (INT i = 0; i < *Count * 3; ++i) v[i] = Float();
  return v;
}

bool ClassIs(const FObjectExport& E, const char* Name) { return appStricmp(*E.ClassName, Name) == 0; }

void AddPoly(LvLevel* lv, const float* base, const float* normal, const float* tu, const float* tv, const float* verts, INT nv) {
  if (lv->count >= LV_MAX_POLYS || nv < 3 || nv > 16) return;
  LvPoly& p = lv->polys[lv->count++];
  p.nv = (unsigned)nv;
  for (INT i = 0; i < 3; ++i) p.base[i] = base[i], p.normal[i] = normal[i], p.tex_u[i] = tu[i], p.tex_v[i] = tv[i];
  for (INT i = 0; i < nv * 3; ++i) p.v[i] = verts[i];
}

// The compiled BSP: node polygons from Nodes/Verts/Points with their surface's texture axes.
bool LoadModel(const FObjectExport& E, LvLevel* lv) {
  Props none = {};
  BeginExport(E, &none);
  for (INT i = 0; i < 25 + 16; ++i) Byte();  // bounding box and sphere
  const INT vectors = Index(), points = Index(), nodes = Index(), surfs = Index(), verts = Index();
  INT nvec, npts;
  float* vec = ReadVectors(vectors, &nvec);
  float* pts = ReadVectors(points, &npts);
  if (!vec || !pts) return false;

  // Surfaces.
  INT ns = BeginDatabase(surfs);
  if (ns <= 0) return false;
  struct Surf { INT base, normal, u, v; };
  Surf* surf = (Surf*)appMalloc(ns * sizeof(Surf), "surf");
  for (INT i = 0; i < ns; ++i) {
    Index();                       // texture
    Dword();                       // poly flags
    surf[i].base = Index(); surf[i].normal = Index(); surf[i].u = Index(); surf[i].v = Index();
    Index(); Index();              // light map, brush poly
    Dword();                       // pan
    Index();                       // actor
  }
  // Vertex pool.
  INT nvert = BeginDatabase(verts);
  if (nvert <= 0) return false;
  INT* vert = (INT*)appMalloc(nvert * sizeof(INT), "verts");
  for (INT i = 0; i < nvert; ++i) { vert[i] = Index(); Index(); }
  // Nodes.
  INT nn = BeginDatabase(nodes);
  if (nn <= 0) return false;
  auto vec_ok = [&](INT i) { return i >= 0 && i < nvec; };
  for (INT i = 0; i < nn; ++i) {
    for (INT k = 0; k < 4; ++k) Float();  // plane
    Dword(); Dword();                      // zone mask
    Byte();                                // flags
    const INT pool = Index(), isurf = Index();
    for (INT k = 0; k < 5; ++k) Index();   // children, bounds
    Byte(); Byte();                        // zones
    const INT count = Byte();
    Dword(); Dword();                      // leaves
    if (count < 3 || isurf < 0 || isurf >= ns || pool < 0 || pool + count > nvert) continue;
    const Surf& sf = surf[isurf];
    if (sf.base < 0 || sf.base >= npts || !vec_ok(sf.normal) || !vec_ok(sf.u) || !vec_ok(sf.v)) continue;
    float p3[16 * 3];
    bool good = true;
    for (INT k = 0; k < count && good; ++k) {
      good = vert[pool + k] >= 0 && vert[pool + k] < npts;
      if (good) for (INT c = 0; c < 3; ++c) p3[k * 3 + c] = pts[vert[pool + k] * 3 + c];
    }
    if (good) AddPoly(lv, &pts[sf.base * 3], &vec[sf.normal * 3], &vec[sf.u * 3], &vec[sf.v * 3], p3, count);
  }
  appFree(vec); appFree(pts); appFree(surf); appFree(vert);
  return lv->count > 0;
}

// Fallback for levels without a compiled BSP: the editor brushes' polygons.
void LoadBrushPolys(const FObjectExport& E, LvLevel* lv) {
  BeginExport(E);
  const INT Count = (INT)Dword();
  Dword();
  for (INT k = 0; k < Count && k < 4096; ++k) {
    INT nv = Index();
    float base[3], normal[3], tu[3], tv[3], v[16 * 3];
    for (INT i = 0; i < 3; ++i) base[i] = Float();
    for (INT i = 0; i < 3; ++i) normal[i] = Float();
    for (INT i = 0; i < 3; ++i) tu[i] = Float();
    for (INT i = 0; i < 3; ++i) tv[i] = Float();
    if (nv < 3 || nv > 16) return;
    for (INT i = 0; i < nv * 3; ++i) v[i] = Float();
    Dword(); Index(); Index(); Index(); Index(); Index(); Dword();
    UBOOL dup = 0;  // the editor stores the same brush more than once
    for (unsigned j = 0; j < lv->count && !dup; ++j)
      if ((INT)lv->polys[j].nv == nv && appMemcmp(lv->polys[j].v, v, nv * 12) == 0) dup = 1;
    if (!dup) AddPoly(lv, base, normal, tu, tv, v, nv);
  }
}

}  // namespace

bool lion_level_load(const char* file, LvLevel* lv) {
  lv->count = 0;
  lv->have_start = 0;
  lv->yaw = 0;
  lv->source = 0;
  L = GObj.GetPackageLinker(NULL, file, LOAD_NoFail | LOAD_NoWarn, NULL, NULL);
  if (!L) return false;
  debugf("level: %i exports, %i names", L->ExportMap.Num(), L->NameMap.Num());
  for (INT i = 0; i < L->ExportMap.Num(); ++i) {
    const FObjectExport& E = L->ExportMap(i);
    if (ClassIs(E, "PlayerStart") && !lv->have_start) {
      Props p = {};
      BeginExport(E, &p);
      if (p.has_location) {
        for (INT k = 0; k < 3; ++k) lv->start[k] = p.location[k];
        lv->yaw = (float)(p.yaw & 0xFFFF) * (6.2831853f / 65536.0f);
        lv->have_start = 1;
      }
    }
  }
  for (INT i = 0; i < L->ExportMap.Num() && !lv->count; ++i)
    if (ClassIs(L->ExportMap(i), "Model") && LoadModel(L->ExportMap(i), lv)) lv->source = "BSP nodes";
  for (INT i = 0; i < L->ExportMap.Num() && !lv->count; ++i)
    if (ClassIs(L->ExportMap(i), "Polys")) LoadBrushPolys(L->ExportMap(i), lv), lv->source = "brush polygons";
  debugf("level: %i polygons from %s", (INT)lv->count, lv->source ? lv->source : "nothing");
  return lv->count > 0;
}
