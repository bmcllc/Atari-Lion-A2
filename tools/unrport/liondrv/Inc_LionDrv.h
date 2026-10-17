// LionDrv: viewport manager, viewport and render device of Unreal on the Atari Lion.
#pragma once
#include "Engine.h"
#include "UnRender.h"

#ifndef LIONDRV_API
#define LIONDRV_API
#endif

// Lion display: 240p, RGB565.
enum { LION_W = 320, LION_H = 240 };

class ULionRenderDevice;

class LIONDRV_API ULionClient : public UClient {
  DECLARE_CLASS_WITHOUT_CONSTRUCT(ULionClient, UClient, CLASS_Transient | CLASS_Config)
  ULionClient();
  static void InternalClassInitializer(UClass* Class);

  void Init(UEngine* InEngine);
  void ShowViewportWindows(DWORD, int) {}
  void EnableViewportWindows(DWORD, int) {}
  void Poll() {}
  UViewport* CurrentViewport() { return Viewports.Num() ? Viewports(0) : NULL; }
  void Tick();
  UBOOL Exec(const char* Cmd, FOutputDevice* Out = GSystem) { return 0; }
  UViewport* NewViewport(class ULevel* InLevel, const FName Name);
  void EndFullscreen() {}
};

class LIONDRV_API ULionViewport : public UViewport {
  DECLARE_CLASS_WITHOUT_CONSTRUCT(ULionViewport, UViewport, CLASS_Transient)
  NO_DEFAULT_CONSTRUCTOR(ULionViewport)

  ULionViewport(ULevel* InLevel, UClient* InClient);
  void Destroy();

  UBOOL Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData = NULL, INT* HitSize = NULL);
  void Unlock(UBOOL Blit = 0);
  void SetModeCursor() {}
  void UpdateWindow() {}
  void OpenWindow(DWORD ParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY);
  void CloseWindow() {}
  void UpdateInput(UBOOL Reset);
  void MakeCurrent() {}
  void* GetWindow() { return this; }
  void SetMouseCapture(UBOOL, UBOOL, UBOOL = 0) {}
  void Repaint();
  void MakeFullscreen(INT, INT, UBOOL) {}

  // Pad state last seen, to turn changes into input events.
  DWORD LastButtons;
  FLOAT LastStick[4];
  UBOOL PadEvent(INT iKey, UBOOL Down);
};

class LIONDRV_API ULionRenderDevice : public URenderDevice {
  DECLARE_CLASS_WITHOUT_CONSTRUCT(ULionRenderDevice, URenderDevice, CLASS_Config)
  ULionRenderDevice();
  static void InternalClassInitializer(UClass* Class);

  UBOOL Init(UViewport* InViewport);
  void Exit();
  void Flush();
  UBOOL Exec(const char* Cmd, FOutputDevice* Out) { return 0; }
  void Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize);
  void Unlock(UBOOL Blit);
  void DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet);
  void DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span);
  void DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags);
  void Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2);
  void Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2);
  void ClearZ(FSceneNode* Frame);
  void PushHit(const BYTE*, INT) {}
  void PopHit(INT, UBOOL) {}
  void GetStats(char* Result) { Result[0] = 0; }
  void ReadPixels(FColor*) {}
};

// Audio on the Tyke DSP: its mixing firmware plays up to 24 PCM16 voices from RAM. Sounds stay on the disc
// (the game package holds 11 MB of them) and are decoded into a small cache the first time they play.
class LIONDRV_API ULionAudioSubsystem : public UAudioSubsystem {
  DECLARE_CLASS_WITHOUT_CONSTRUCT(ULionAudioSubsystem, UAudioSubsystem, CLASS_Config)
  ULionAudioSubsystem();
  UBOOL Init();
  void SetViewport(UViewport* Viewport);
  UBOOL Exec(const char* Cmd, FOutputDevice* Out = GSystem);
  void Update(FPointRegion Region, FCoords& Listener);
  void RegisterMusic(UMusic*) {}
  void RegisterSound(USound* Sound);
  void UnregisterSound(USound* Sound);
  void UnregisterMusic(UMusic*) {}
  UBOOL PlaySound(AActor* Actor, INT Id, USound* Sound, FVector Location, FLOAT Volume, FLOAT Radius, FLOAT Pitch);
  void NoteDestroy(AActor* Actor);
  UBOOL GetLowQualitySetting() { return 1; }

  UViewport* Viewport;
  INT Ticks;
};
