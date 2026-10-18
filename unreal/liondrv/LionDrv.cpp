// LionDrv implementation. The render device draws with the Spike (see LionRenDev.cpp).
#include "Inc_LionDrv.h"

/*-----------------------------------------------------------------------------
	ULionClient.
-----------------------------------------------------------------------------*/

ULionClient::ULionClient() {}

void ULionClient::InternalClassInitializer(UClass* Class) {
  guard(ULionClient::InternalClassInitializer);
  UClient::InternalClassInitializer(Class);
  unguard;
}

void ULionClient::Init(UEngine* InEngine) {
  guard(ULionClient::Init);
  UClient::Init(InEngine);
  ViewportX = LION_W;
  ViewportY = LION_H;
  debugf(NAME_Init, "Lion client initialized");
  unguard;
}

UViewport* ULionClient::NewViewport(ULevel* InLevel, const FName Name) {
  return new (GObj.GetTransientPackage(), Name) ULionViewport(InLevel, this);
}

// Realtime viewports repaint every tick.
void ULionClient::Tick() {
  for (INT i = 0; i < Viewports.Num(); ++i) {
    UViewport* V = Viewports(i);
    if (V->IsRealtime() && V->SizeX && V->SizeY && !V->OnHold) V->Repaint();
  }
}
IMPLEMENT_CLASS(ULionClient);

/*-----------------------------------------------------------------------------
	ULionViewport.
-----------------------------------------------------------------------------*/

ULionViewport::ULionViewport(ULevel* InLevel, UClient* InClient) : UViewport(InLevel, InClient) {
  guard(ULionViewport::ULionViewport);
  LastButtons = 0;
  for (INT i = 0; i < 4; ++i) LastStick[i] = 0;
  unguard;
}

void ULionViewport::Destroy() {
  UViewport::Destroy();
}

static void TryRenderDevice(UViewport* Viewport, const char* ClassName) {
  UClass* Cls = GObj.LoadClass(URenderDevice::StaticClass, NULL, ClassName, NULL, LOAD_NoWarn | LOAD_KeepImports, NULL);
  if (Cls) {
    Viewport->RenDev = ConstructClassObject<URenderDevice>(Cls);
    if (!Viewport->RenDev->Init(Viewport)) {
      delete Viewport->RenDev;
      Viewport->RenDev = NULL;
    }
  }
}

void ULionViewport::OpenWindow(DWORD, UBOOL, INT NewX, INT NewY, INT, INT) {
  guard(ULionViewport::OpenWindow);
  check(Actor);
  SizeX = LION_W;
  SizeY = LION_H;
  ColorBytes = 2;
  Caps |= CC_RGB565;
  if (!RenDev) {
    TryRenderDevice(this, "ini:Engine.Engine.GameRenderDevice");
    check(RenDev);
  }
  debugf(NAME_Log, "Opened Lion viewport %ix%i", SizeX, SizeY);
  unguard;
}

UBOOL ULionViewport::Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize) {
  Stride = SizeX;
  ScreenPointer = NULL;  // the Spike draws straight into the SGRAM frame buffer
  return UViewport::Lock(FlashScale, FlashFog, ScreenClear, RenderLockFlags, HitData, HitSize);
}

void ULionViewport::Unlock(UBOOL Blit) {
  UViewport::Unlock(Blit);
}

void ULionViewport::Repaint() {
  if (!OnHold && RenDev && SizeX && SizeY) Client->Engine->Draw(this, 0);
}

// Pad -> Unreal input keys (the Joy keys carry the stock Unreal bindings).
#include "lionpad.inc"

IMPLEMENT_CLASS(ULionViewport);

IMPLEMENT_PACKAGE(LionDrv);
