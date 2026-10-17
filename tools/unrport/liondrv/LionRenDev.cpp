// The render device of the Lion: draws BSP surfaces, meshes and tiles with the Spike.
//
// Textures: Unreal's 8-bit palettized (and RGBA7) mipmaps are converted to RGB565 and cached in the
// SGRAM texture pool, keyed by the engine's cache ID. Surfaces are drawn Z-buffered, perspective
// correct, mipmapped and bilinear; masked textures use the Spike colour key.
#include "Inc_LionDrv.h"

#define SPIKE_CMD (*(volatile DWORD*)0x1F010000)
#define SPIKE_SYNC (*(volatile DWORD*)0x1F010008)
#define VI_MODE (*(volatile DWORD*)0x1F030000)
#define VI_FB (*(volatile DWORD*)0x1F030004)
#define VI_STRIDE (*(volatile DWORD*)0x1F030008)
#define VI_FRAME (*(volatile DWORD*)0x1F030010)

enum { OP_SET_TARGET = 1, OP_CLEAR = 2, OP_SET_MODE = 3, OP_SET_TEXTURE = 4, OP_TRI = 5, OP_SYNC = 6 };
enum {
  M_ZTEST = 1, M_ZWRITE = 2, M_GOURAUD = 4, M_TEXTURE = 8, M_BILINEAR = 16, M_PERSP = 32, M_BLEND = 64,
  M_MIPMAP = 256, M_TRILINEAR = 512, M_BLENDADD = 1024, M_COLORKEY = 2048
};
enum : DWORD { kBuf0 = 0x10000000, kBuf1 = 0x10100000, kZBuf = 0x10200000, kPool = 0x10400000, kPoolEnd = 0x10800000 };

static inline void Put(DWORD v) { SPIKE_CMD = v; }
static inline DWORD Bits(FLOAT f) { union { FLOAT f; DWORD u; } x; x.f = f; return x.u; }

/*-----------------------------------------------------------------------------
	Frame state.
-----------------------------------------------------------------------------*/

static DWORD GFront, GBack, GToken, GMode, GBoundTex;

static void WaitIdle() {
  Put(OP_SYNC << 24 | 1);
  Put(++GToken);
  while (SPIKE_SYNC != GToken) {}
}

static void SetMode(DWORD Mode) {
  if (Mode == GMode) return;
  GMode = Mode;
  Put(OP_SET_MODE << 24 | 1);
  Put(Mode);
}

/*-----------------------------------------------------------------------------
	Texture cache.
-----------------------------------------------------------------------------*/

struct FLionTex {
  QWORD Id, PalId;
  DWORD Masked;
  DWORD Addr, Log2W, Log2H, Levels;
  FLOAT InvW, InvH;  // normalised coordinates per texel of the base level
};
enum { MAX_TEX = 384 };
static FLionTex GTex[MAX_TEX];
static INT GTexCount;
static DWORD GPoolNext = kPool;
static FLionTex* GBoundEntry;

static DWORD Log2(DWORD V) { DWORD n = 0; while ((1u << n) < V) ++n; return n; }

static inline _WORD To565(BYTE R, BYTE G, BYTE B) { return (_WORD)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)); }

static FLionTex* UploadTexture(const FTextureInfo& Info, UBOOL Masked) {
  if (GTexCount >= MAX_TEX) { WaitIdle(); GTexCount = 0; GPoolNext = kPool; GBoundEntry = NULL; GBoundTex = 0; }
  // Size of the chain.
  DWORD Bytes = 0;
  INT Levels = 0;
  for (INT m = 0; m < Info.NumMips && m < MAX_MIPS; ++m) {
    FMipmap* M = Info.Mips[m];
    if (!M || !M->DataPtr || M->USize > 1024 || M->VSize > 1024) break;
    Bytes += M->USize * M->VSize * 2;
    ++Levels;
  }
  if (!Levels) return NULL;
  if (GPoolNext + Bytes > kPoolEnd) { WaitIdle(); GTexCount = 0; GPoolNext = kPool; GBoundEntry = NULL; GBoundTex = 0; }
  if (GPoolNext + Bytes > kPoolEnd) return NULL;
  FLionTex& T = GTex[GTexCount++];
  T.Id = Info.CacheID; T.PalId = Info.PaletteCacheID; T.Masked = Masked;
  T.Addr = GPoolNext;
  T.Log2W = Log2(Info.Mips[0]->USize); T.Log2H = Log2(Info.Mips[0]->VSize); T.Levels = Levels;
  T.InvW = 1.0f / (Info.UScale * Info.USize); T.InvH = 1.0f / (Info.VScale * Info.VSize);
  volatile _WORD* Out = (volatile _WORD*)GPoolNext;
  for (INT m = 0; m < Levels; ++m) {
    FMipmap* M = Info.Mips[m];
    const INT N = M->USize * M->VSize;
    if (Info.Format == TEXF_RGB32) {
      const FColor* P = (const FColor*)M->DataPtr;
      for (INT i = 0; i < N; ++i) {
        _WORD C = To565(P[i].R, P[i].G, P[i].B);
        if (Masked) C = P[i].A ? (C ? C : 1) : 0;
        *Out++ = C;
      }
    } else {
      const BYTE* P = M->DataPtr;
      const FColor* Pal = Info.Palette;
      for (INT i = 0; i < N; ++i) {
        const BYTE Ix = P[i];
        _WORD C = Pal ? To565(Pal[Ix].R, Pal[Ix].G, Pal[Ix].B) : To565(Ix, Ix, Ix);
        if (Masked) C = Ix ? (C ? C : 1) : 0;  // index 0 is the transparent colour of masked textures
        *Out++ = C;
      }
    }
  }
  GPoolNext += Bytes;
  GPoolNext = (GPoolNext + 31) & ~31u;
  return &T;
}

static FLionTex* FindTexture(const FTextureInfo& Info, UBOOL Masked) {
  for (INT i = 0; i < GTexCount; ++i)
    if (GTex[i].Id == Info.CacheID && GTex[i].PalId == Info.PaletteCacheID && GTex[i].Masked == (DWORD)Masked) return &GTex[i];
  return UploadTexture(Info, Masked);
}

static void BindTexture(FLionTex* T) {
  if (GBoundEntry == T) return;
  GBoundEntry = T;
  Put(OP_SET_TEXTURE << 24 | 4);
  Put(T->Addr);
  Put(T->Log2W);
  Put(T->Log2H);
  Put(T->Levels);
}

/*-----------------------------------------------------------------------------
	Primitives.
-----------------------------------------------------------------------------*/

struct FLionVertex { FLOAT X, Y, Z, RW; DWORD Argb; FLOAT U, V; };

static inline void EmitVertex(const FLionVertex& V) {
  Put(Bits(V.X)); Put(Bits(V.Y)); Put(Bits(V.Z)); Put(Bits(V.RW)); Put(V.Argb); Put(Bits(V.U)); Put(Bits(V.V));
}
static inline void EmitTri(const FLionVertex& A, const FLionVertex& B, const FLionVertex& C) {
  Put(OP_TRI << 24 | 21);
  EmitVertex(A); EmitVertex(B); EmitVertex(C);
}

// Depth for the Z buffer: 0 at the near plane, 1 at the far plane, linear in screen space after the divide.
static inline FLOAT Depth(FLOAT CameraZ) {
  const FLOAT Near = 1.0f, Far = 65536.0f;
  FLOAT D = (1.0f - Near / CameraZ) * (1.0f / (1.0f - Near / Far));
  return D < 0 ? 0 : D > 1 ? 1 : D;
}

static inline DWORD PackColor(FLOAT R, FLOAT G, FLOAT B, FLOAT A) {
  auto C = [](FLOAT V) { INT I = (INT)(V * 255.0f); return (DWORD)(I < 0 ? 0 : I > 255 ? 255 : I); };
  return C(A) << 24 | C(R) << 16 | C(G) << 8 | C(B);
}

static DWORD ModeFor(DWORD PolyFlags, UBOOL Textured, UBOOL DepthWrite) {
  DWORD M = M_ZTEST | M_GOURAUD | M_PERSP;
  if (DepthWrite) M |= M_ZWRITE;
  if (Textured) {
    M |= M_TEXTURE | M_MIPMAP | M_TRILINEAR;
    if (!(PolyFlags & PF_NoSmooth)) M |= M_BILINEAR;
    if (PolyFlags & PF_Masked) M |= M_COLORKEY;
  }
  if (PolyFlags & PF_Translucent) M = (M & ~M_ZWRITE) | M_BLENDADD;
  else if (PolyFlags & PF_Modulated) M = (M & ~M_ZWRITE) | M_BLEND;
  return M;
}

/*-----------------------------------------------------------------------------
	ULionRenderDevice.
-----------------------------------------------------------------------------*/

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
  GFront = kBuf0;
  GBack = kBuf1;
  VI_MODE = 1;  // 240p
  VI_FB = GFront;
  VI_STRIDE = LION_W * 2;
  debugf(NAME_Init, "Lion render device initialized");
  return 1;
}

void ULionRenderDevice::Exit() {}

void ULionRenderDevice::Flush() {
  WaitIdle();
  GTexCount = 0;
  GPoolNext = kPool;
  GBoundEntry = NULL;
}

void ULionRenderDevice::Lock(FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE*, INT*) {
  Put(OP_SET_TARGET << 24 | 5);
  Put(GBack);
  Put(LION_W * 2);
  Put(LION_W);
  Put(LION_H);
  Put(kZBuf);
  const _WORD Clear = To565((BYTE)Clamp(ScreenClear.X * 255.0f, 0.0f, 255.0f), (BYTE)Clamp(ScreenClear.Y * 255.0f, 0.0f, 255.0f), (BYTE)Clamp(ScreenClear.Z * 255.0f, 0.0f, 255.0f));
  Put(OP_CLEAR << 24 | 3);
  Put((RenderLockFlags & LOCKR_ClearScreen) ? 3 : 2);
  Put(Clear);
  Put(0xFFFFFF);
  GMode = 0xFFFFFFFF;  // force the next SetMode
  GBoundEntry = NULL;
}

void ULionRenderDevice::Unlock(UBOOL Blit) {
  if (!Blit) return;
  WaitIdle();
  VI_FB = GBack;
  const DWORD T = GFront;
  GFront = GBack;
  GBack = T;
}

void ULionRenderDevice::ClearZ(FSceneNode*) {
  Put(OP_CLEAR << 24 | 3);
  Put(2);
  Put(0);
  Put(0xFFFFFF);
}

void ULionRenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet) {
  if (Surface.PolyFlags & PF_Invisible) return;
  FLionTex* Tex = Surface.Texture ? FindTexture(*Surface.Texture, (Surface.PolyFlags & PF_Masked) != 0) : NULL;
  SetMode(ModeFor(Surface.PolyFlags, Tex != NULL, 1));
  if (Tex) BindTexture(Tex);
  const FLOAT UDot = Facet.MapCoords.XAxis | Facet.MapCoords.Origin;
  const FLOAT VDot = Facet.MapCoords.YAxis | Facet.MapCoords.Origin;
  const FLOAT PanX = Surface.Texture ? Surface.Texture->Pan.X : 0, PanY = Surface.Texture ? Surface.Texture->Pan.Y : 0;
  const DWORD Argb = (Surface.PolyFlags & PF_Unlit) ? 0xFFFFFFFF : 0xFFC8C8C8;
  for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next) {
    if (Poly->NumPts < 3) continue;
    FLionVertex V[32];
    const INT N = Poly->NumPts < 32 ? Poly->NumPts : 32;
    for (INT i = 0; i < N; ++i) {
      const FTransform* P = Poly->Pts[i];
      V[i].X = P->ScreenX;
      V[i].Y = P->ScreenY;
      V[i].Z = Depth(P->Point.Z);
      V[i].RW = 1.0f / P->Point.Z;
      V[i].Argb = Argb;
      if (Tex) {
        V[i].U = ((Facet.MapCoords.XAxis | P->Point) - UDot - PanX) * Tex->InvW;
        V[i].V = ((Facet.MapCoords.YAxis | P->Point) - VDot - PanY) * Tex->InvH;
      } else {
        V[i].U = V[i].V = 0;
      }
    }
    for (INT i = 1; i + 1 < N; ++i) EmitTri(V[0], V[i], V[i + 1]);
  }
}

void ULionRenderDevice::DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer*) {
  if (NumPts < 3) return;
  FLionTex* Tex = FindTexture(Info, (PolyFlags & PF_Masked) != 0);
  SetMode(ModeFor(PolyFlags, Tex != NULL, !(PolyFlags & (PF_Translucent | PF_Modulated))));
  if (Tex) BindTexture(Tex);
  FLionVertex V[32];
  const INT N = NumPts < 32 ? NumPts : 32;
  for (INT i = 0; i < N; ++i) {
    const FTransTexture* P = Pts[i];
    V[i].X = P->ScreenX;
    V[i].Y = P->ScreenY;
    V[i].Z = Depth(P->Point.Z);
    V[i].RW = 1.0f / P->Point.Z;
    V[i].Argb = (PolyFlags & PF_Unlit) ? 0xFFFFFFFF : PackColor(P->Light.X, P->Light.Y, P->Light.Z, 1.0f);
    V[i].U = Tex ? P->U * Tex->InvW : 0;
    V[i].V = Tex ? P->V * Tex->InvH : 0;
  }
  for (INT i = 1; i + 1 < N; ++i) EmitTri(V[0], V[i], V[i + 1]);
}

void ULionRenderDevice::DrawTile(FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer*, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags) {
  FLionTex* Tex = FindTexture(Info, (PolyFlags & PF_Masked) != 0);
  DWORD M = (Z > 0 ? M_ZTEST : 0) | M_PERSP | M_GOURAUD;
  if (Tex) { M |= M_TEXTURE; if (!(PolyFlags & PF_NoSmooth)) M |= M_BILINEAR; if (PolyFlags & PF_Masked) M |= M_COLORKEY; }
  if (PolyFlags & PF_Translucent) M |= M_BLENDADD;
  else if (PolyFlags & PF_Modulated) M |= M_BLEND;
  SetMode(M);
  if (Tex) BindTexture(Tex);
  const FLOAT Zd = Z > 0 ? Depth(Z) : 0, Rw = Z > 0 ? 1.0f / Z : 1.0f;
  const DWORD Argb = PackColor(Color.X, Color.Y, Color.Z, (PolyFlags & (PF_Translucent | PF_Modulated)) ? Color.W : 1.0f);
  const FLOAT U0 = Tex ? U * Tex->InvW : 0, V0 = Tex ? V * Tex->InvH : 0;
  const FLOAT U1 = Tex ? (U + UL) * Tex->InvW : 0, V1 = Tex ? (V + VL) * Tex->InvH : 0;
  FLionVertex A = {X, Y, Zd, Rw, Argb, U0, V0}, B = {X + XL, Y, Zd, Rw, Argb, U1, V0};
  FLionVertex C = {X + XL, Y + YL, Zd, Rw, Argb, U1, V1}, D = {X, Y + YL, Zd, Rw, Argb, U0, V1};
  EmitTri(A, B, C);
  EmitTri(A, C, D);
}

void ULionRenderDevice::Draw2DLine(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2) {
  SetMode(M_GOURAUD);
  const FLOAT Dx = P2.X - P1.X, Dy = P2.Y - P1.Y;
  const FLOAT Len = appSqrt(Dx * Dx + Dy * Dy);
  if (Len < 0.01f) return;
  const FLOAT Nx = -Dy / Len * 0.5f, Ny = Dx / Len * 0.5f;
  const DWORD Argb = PackColor(Color.X, Color.Y, Color.Z, 1.0f);
  FLionVertex A = {P1.X + Nx, P1.Y + Ny, 0, 1, Argb, 0, 0}, B = {P1.X - Nx, P1.Y - Ny, 0, 1, Argb, 0, 0};
  FLionVertex C = {P2.X - Nx, P2.Y - Ny, 0, 1, Argb, 0, 0}, D = {P2.X + Nx, P2.Y + Ny, 0, 1, Argb, 0, 0};
  EmitTri(A, B, C);
  EmitTri(A, C, D);
}

void ULionRenderDevice::Draw2DPoint(FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2) {
  SetMode(M_GOURAUD);
  const DWORD Argb = PackColor(Color.X, Color.Y, Color.Z, 1.0f);
  FLionVertex A = {X1, Y1, 0, 1, Argb, 0, 0}, B = {X2, Y1, 0, 1, Argb, 0, 0};
  FLionVertex C = {X2, Y2, 0, 1, Argb, 0, 0}, D = {X1, Y2, 0, 1, Argb, 0, 0};
  EmitTri(A, B, C);
  EmitTri(A, C, D);
}

IMPLEMENT_CLASS(ULionRenderDevice);
