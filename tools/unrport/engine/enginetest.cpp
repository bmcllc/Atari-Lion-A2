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
