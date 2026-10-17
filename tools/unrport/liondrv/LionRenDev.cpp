// The render device of the Lion: draws BSP surfaces, meshes and tiles with the Spike.
#include "Inc_LionDrv.h"

ULionRenderDevice::ULionRenderDevice() {}

void ULionRenderDevice::InternalClassInitializer(UClass* Class) {
  guard(ULionRenderDevice::InternalClassInitializer);
  URenderDevice::InternalClassInitializer(Class);
  unguard;
}

UBOOL ULionRenderDevice::Init(UViewport* InViewport) {
  Viewport = InViewport;
  SpanBased = 0;
  FrameBuffered = 0;
  SupportsFogMaps = 0;
  SupportsDistanceFog = 0;
  VolumetricLighting = 0;
  ShinySurfaces = 0;
  Coronas = 0;
  HighDetailActors = 0;
  debugf(NAME_Init, "Lion render device initialized");
  return 1;
}
void ULionRenderDevice::Exit() {}
void ULionRenderDevice::Flush() {}
void ULionRenderDevice::Lock(FPlane, FPlane, FPlane, DWORD, BYTE*, INT*) {}
void ULionRenderDevice::Unlock(UBOOL) {}
void ULionRenderDevice::DrawComplexSurface(FSceneNode*, FSurfaceInfo&, FSurfaceFacet&) {}
void ULionRenderDevice::DrawGouraudPolygon(FSceneNode*, FTextureInfo&, FTransTexture**, int, DWORD, FSpanBuffer*) {}
void ULionRenderDevice::DrawTile(FSceneNode*, FTextureInfo&, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FSpanBuffer*, FLOAT, FPlane, FPlane, DWORD) {}
void ULionRenderDevice::Draw2DLine(FSceneNode*, FPlane, DWORD, FVector, FVector) {}
void ULionRenderDevice::Draw2DPoint(FSceneNode*, FPlane, DWORD, FLOAT, FLOAT, FLOAT, FLOAT) {}
void ULionRenderDevice::ClearZ(FSceneNode*) {}
IMPLEMENT_CLASS(ULionRenderDevice);
