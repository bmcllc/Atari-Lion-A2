// Smoke test of the real Engine on the Lion: load Engine.u and look at a native class.
#include "Engine.h"

// Dumps the script-side layout of a class so it can be compared with the C++ struct.
static void DumpClass(const char* Name) {
  UClass* C = (UClass*)GObj.LoadObject(UClass::StaticClass, NULL, Name, NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  INT n = 0;
  for (UField* F = C->Children; F; F = F->Next) {
    UProperty* P = Cast<UProperty>(F);
    if (P) debugf("prop %s off=%i dim=%i size=%i", P->GetName(), P->Offset, P->ArrayDim, P->GetElementSize()), ++n;
  }
  debugf("dump %s: %i properties, %i bytes", Name, n, C->GetPropertiesSize());
}

// Native class sizes are recorded before Engine.u is loaded, then compared with the script-side sizes:
// a mismatch means the C++ struct and the script disagree about the layout (padding, bitfields, packing).
struct NativeSize { FName Name; INT Size; };
static TArray<NativeSize> GNativeSizes;

static void SnapshotNativeSizes() {
  for (FObjectIterator It(UClass::StaticClass); It; ++It) {
    UClass* C = (UClass*)*It;
    NativeSize& N = *new(GNativeSizes) NativeSize;
    N.Name = C->GetFName();
    N.Size = C->GetPropertiesSize();
  }
}

extern "C" void lion_check_class_sizes() {
  DumpClass("Fire.FractalTexture");
  DumpClass("Fire.FireTexture");
  INT Bad = 0;
  for (FObjectIterator It(UClass::StaticClass); It; ++It) {
    UClass* C = (UClass*)*It;
    for (INT i = 0; i < GNativeSizes.Num(); ++i)
      if (GNativeSizes(i).Name == C->GetFName() && GNativeSizes(i).Size != C->GetPropertiesSize() && C->Constructor) {
        debugf("SIZE MISMATCH %s: script %i native %i", C->GetName(), C->GetPropertiesSize(), GNativeSizes(i).Size);
        ++Bad;
      }
  }
  debugf("class size check: %i mismatches among %i native classes", Bad, GNativeSizes.Num());
}

extern "C" void lion_engine_test() {
  SnapshotNativeSizes();
  debugf("engine test: loading Engine.Actor");
  UClass* C = (UClass*)GObj.LoadObject(UClass::StaticClass, NULL, "Engine.Actor", NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  if (!C) appErrorf("Engine.Actor not found");
  debugf("engine test: %s, %i bytes of properties, super %s", C->GetName(), C->GetPropertiesSize(), C->GetSuperClass() ? C->GetSuperClass()->GetName() : "none");
  debugf("engine test: sizeof(AActor)=%i", (INT)sizeof(AActor));
}

// Memory census (a debugging aid): where the object heap goes, by kind.
extern "C" void lion_tag_report();
extern "C" void lion_class_report();
extern "C" void lion_census(const char* Tag) {
  INT Tex = 0, TexBytes = 0, Meshes = 0, MeshBytes = 0, Funcs = 0, ScriptBytes = 0, Total = 0, Others = 0;
  for (FObjectIterator It; It; ++It) {
    UObject* O = *It;
    ++Total;
    if (O->IsA(UTexture::StaticClass)) {
      UTexture* T = (UTexture*)O;
      ++Tex;
      for (INT i = 0; i < T->Mips.Num(); ++i) TexBytes += T->Mips(i).DataArray.Num();
    } else if (O->IsA(UMesh::StaticClass)) {
      UMesh* M = (UMesh*)O;
      ++Meshes;
      MeshBytes += M->Verts.Num() * sizeof(FMeshVert) + M->Tris.Num() * sizeof(FMeshTri) + M->VertLinks.Num() * 4 +
                   M->Connects.Num() * sizeof(FMeshVertConnect) + M->AnimSeqs.Num() * sizeof(FMeshAnimSeq);
    } else if (O->IsA(UStruct::StaticClass)) {
      ++Funcs;
      ScriptBytes += ((UStruct*)O)->Script.Num();
    } else {
      ++Others;
    }
  }
  debugf("census %s: %i objects; %i textures %i KB; %i meshes %i KB; %i structs/classes script %i KB; %i others",
         Tag, Total, Tex, TexBytes >> 10, Meshes, MeshBytes >> 10, Funcs, ScriptBytes >> 10, Others);
  // Instance sizes by class.
  static const char* Names[40];
  static INT Count[40], Bytes[40];
  INT N = 0;
  for (FObjectIterator It; It; ++It) {
    const char* Name = It->GetClass()->GetName();
    INT k = 0;
    for (; k < N; ++k) if (Names[k] == Name || !appStrcmp(Names[k], Name)) break;
    if (k == N) { if (N == 40) continue; Names[N++] = Name; Count[k] = Bytes[k] = 0; }
    ++Count[k];
    Bytes[k] += It->GetClass()->GetPropertiesSize();
  }
  for (INT r = 0; r < 8; ++r) {
    INT Best = -1;
    for (INT k = 0; k < N; ++k) if (Bytes[k] > 0 && (Best < 0 || Bytes[k] > Bytes[Best])) Best = k;
    if (Best < 0) break;
    debugf("census class %s: %i objects, %i KB instances", Names[Best], Count[Best], Bytes[Best] >> 10);
    Bytes[Best] = 0;
  }
  lion_tag_report();
  lion_class_report();
}

// Called while a texture loads: whether to drop its top mipmap (see patch_engine.py). The first megabyte of
// full-size textures is kept as it is; after that, large ones lose their top level and so three quarters of
// their memory.
extern "C" int lion_drop_top_mip(UTexture* Texture) {
  static INT Kept;
  const INT Bytes = Texture->Mips(0).DataArray.Num();
  if (Texture->USize < 128 && Texture->VSize < 128) return 0;
  if (Kept + Bytes <= (1 << 20)) { Kept += Bytes; return 0; }
  return 1;
}
