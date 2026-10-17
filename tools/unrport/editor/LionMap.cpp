// The level of the Lion port, built on the device with the real editor code (CSG, BSP, visibility, lighting):
// the repository ships no maps, so rooms are carved out of a solid world with box brushes, textured with
// procedural textures, lit by static lights and populated with game actors.
#include "EditorPrivate.h"

extern "C" void lion_heap_stats(DWORD* Used, DWORD* ArenaLeft);
extern "C" void lion_census(const char* Tag);
static void Mem(const char* Where) {
  DWORD Used, Left;
  lion_heap_stats(&Used, &Left);
  debugf("map: mem %s: %i KB in use, arena left %i KB", Where, (INT)(Used >> 10), (INT)(Left >> 10));
}



/*-----------------------------------------------------------------------------
	Procedural textures (the repository has no wall textures).
-----------------------------------------------------------------------------*/

enum ETexKind { TK_Brick, TK_Stone, TK_Panel, TK_Trim, TK_Tech };

static INT Hash2(INT X, INT Y, INT Seed) {
  DWORD H = DWORD(X) * 374761393u + DWORD(Y) * 668265263u + DWORD(Seed) * 2147483647u;
  H = (H ^ (H >> 13)) * 1274126177u;
  return INT((H ^ (H >> 16)) & 0xFFFF);
}

static INT Clamp255(INT V) { return V < 0 ? 0 : V > 255 ? 255 : V; }

// Brightness (0..255) of one texel.
static INT Texel(ETexKind Kind, INT X, INT Y, INT Size) {
  const INT N = Hash2(X, Y, 7) % 25 - 12;
  switch (Kind) {
    case TK_Brick: {
      const INT Row = Y / 16, BX = (X + (Row & 1) * 16) & 31, BY = Y & 15;
      if (BX < 2 || BY < 2) return 40 + N / 2;                              // mortar
      const INT Brick = Hash2((X + (Row & 1) * 16) / 32, Row, 3) % 60 - 30;  // per-brick tint
      INT V = 150 + Brick + N;
      if (BX < 4 || BY < 4) V += 18;                                        // lit edge
      if (BX > 29 || BY > 13) V -= 18;                                      // shaded edge
      return Clamp255(V);
    }
    case TK_Stone: {
      const INT TX = X & 63, TY = Y & 63;
      if (TX < 2 || TY < 2) return 50 + N / 2;
      INT V = 150 + (Hash2(X / 64, Y / 64, 5) % 50 - 25) + N;
      if (Hash2(X, Y, 11) % 64 == 0) V -= 50;  // pits
      return Clamp255(V);
    }
    case TK_Panel: {
      const INT TX = X & 63, TY = Y & 63;
      if (TX < 3 || TY < 3 || TX > 60 || TY > 60) return 70 + N / 2;
      INT V = 140 + N / 2 + (TY < 6 ? 25 : 0);
      if ((TX == 8 || TX == 55) && (TY == 8 || TY == 55)) V = 230;  // rivets
      return Clamp255(V);
    }
    case TK_Trim: {
      INT V = 150 + N;
      if ((Y & 31) < 3) V = 80;
      if ((Y & 31) >= 28) V = 210;
      return Clamp255(V);
    }
    default: {  // TK_Tech
      const INT TX = X & 31, TY = Y & 63;
      INT V = 100 + N / 2;
      if (TX < 2 || TY < 2) V = 45;
      if (TY > 26 && TY < 34) V = 220;  // light strip
      if (TX == 15 && TY > 40) V = 160;
      return Clamp255(V);
    }
  }
}

static UTexture* MakeTexture(const char* Name, ETexKind Kind, INT Size, INT R0, INT G0, INT B0, INT R1, INT G1, INT B1) {
  UTexture* Tex = new (GObj.GetTransientPackage(), Name) UTexture;
  Tex->Init(Size, Size);
  Tex->Palette = new (GObj.GetTransientPackage(), NAME_None) UPalette;
  Tex->Palette->Colors.Add(256);
  for (INT i = 0; i < 256; ++i) {
    const INT T = i, U = 255 - i;
    Tex->Palette->Colors(i) = FColor((R0 * U + R1 * T) / 255, (G0 * U + G1 * T) / 255, (B0 * U + B1 * T) / 255, 255);
  }
  for (INT Y = 0; Y < Size; ++Y)
    for (INT X = 0; X < Size; ++X) Tex->Mips(0).DataArray(Y * Size + X) = (BYTE)Texel(Kind, X, Y, Size);
  Tex->CreateMips(1, 1);
  Tex->CreateColorRange();
  return Tex;
}

/*-----------------------------------------------------------------------------
	Brushes.
-----------------------------------------------------------------------------*/

namespace {

struct Textures {
  UTexture *Wall, *Floor, *Ceil, *Trim;
};

struct Builder {
  UEditorEngine* Ed;
  ULevel* Level;
  Textures Tex;
  INT Brushes;
};

FVector Corner(const FVector& Min, const FVector& Max, INT i) {
  return FVector((i & 1) ? Max.X : Min.X, (i & 2) ? Max.Y : Min.Y, (i & 4) ? Max.Z : Min.Z);
}

// One quad of the box with the given outward normal axis (0..5 = -X +X -Y +Y -Z +Z).
void AddFace(UPolys* Polys, const FVector& Min, const FVector& Max, INT Face, UTexture* Texture) {
  static const INT Corners[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
  static const FVector Outward[6] = {FVector(-1, 0, 0), FVector(1, 0, 0), FVector(0, -1, 0), FVector(0, 1, 0), FVector(0, 0, -1), FVector(0, 0, 1)};
  FPoly P;
  P.Init();
  P.NumVertices = 4;
  for (INT i = 0; i < 4; ++i) P.Vertex[i] = Corner(Min, Max, Corners[Face][i]);
  P.CalcNormal();
  if ((P.Normal | Outward[Face]) < 0) {
    P.Reverse();
    P.CalcNormal();
  }
  P.Base = P.Vertex[0];
  // One texel per world unit, upright on walls.
  if (Face < 4) {
    P.TextureU = (Face < 2) ? FVector(0, 1, 0) : FVector(1, 0, 0);
    P.TextureV = FVector(0, 0, -1);
  } else {
    P.TextureU = FVector(1, 0, 0);
    P.TextureV = FVector(0, 1, 0);
  }
  P.Texture = Texture;
  Polys->AddItem(P);
}

// Subtracts (carves) or adds a box. Carved boxes get wall/floor/ceiling textures; added ones trim and floor.
void Box(Builder& B, FLOAT X0, FLOAT Y0, FLOAT Z0, FLOAT X1, FLOAT Y1, FLOAT Z1, ECsgOper Oper) {
  ABrush* Builder = B.Level->Brush();
  UPolys* Polys = Builder->Brush->Polys;
  Polys->Empty();
  const FVector Min(X0, Y0, Z0), Max(X1, Y1, Z1);
  for (INT Face = 0; Face < 6; ++Face) {
    UTexture* T;
    if (Oper == CSG_Subtract) T = Face < 4 ? B.Tex.Wall : Face == 4 ? B.Tex.Floor : B.Tex.Ceil;
    else T = Face < 4 ? B.Tex.Trim : Face == 5 ? B.Tex.Floor : B.Tex.Trim;
    AddFace(Polys, Min, Max, Face, T);
  }
  Builder->Brush->BuildBound();
  B.Ed->csgAddOperation(Builder, B.Level, 0, Oper);
  Polys->Empty();
  ++B.Brushes;
}

void Carve(Builder& B, FLOAT X0, FLOAT Y0, FLOAT Z0, FLOAT X1, FLOAT Y1, FLOAT Z1) { Box(B, X0, Y0, Z0, X1, Y1, Z1, CSG_Subtract); }
void Solid(Builder& B, FLOAT X0, FLOAT Y0, FLOAT Z0, FLOAT X1, FLOAT Y1, FLOAT Z1) { Box(B, X0, Y0, Z0, X1, Y1, Z1, CSG_Add); }

/*-----------------------------------------------------------------------------
	The floor plan. Z=0 is the floor of every room.

	             [D hall]----[C library]
	                 |
	  [E pit]--[A start]--[B great hall]
	                 |
	             [F armoury]
-----------------------------------------------------------------------------*/

void BuildRooms(Builder& B) {
  // The solid world.
  Solid(B, -4096, -4096, -512, 4096, 4096, 1024);

  // A: start room.
  Carve(B, -256, -256, 0, 256, 256, 224);
  // B: great hall to the east with a central dais and four pillars.
  Carve(B, 576, -384, 0, 1344, 384, 352);
  Carve(B, 256 - 8, -64, 0, 576 + 8, 64, 144);  // corridor A-B
  // C: library to the north-east, reached from D.
  Carve(B, -256, 768, 0, 256, 1280, 224);
  // D: hall to the north.
  Carve(B, -64, 256 - 8, 0, 64, 768 + 8, 144);  // corridor A-C
  // E: west room with a sunken pit.
  Carve(B, -1024, -320, 0, -576, 320, 256);
  Carve(B, -576 - 8, -64, 0, -256 + 8, 64, 144);  // corridor A-E
  Carve(B, -928, -224, -96, -672, 224, 0);        // the pit
  // F: armoury to the south.
  Carve(B, -192, -832, 0, 192, -384, 192);
  Carve(B, -64, -384 - 8, 0, 64, -256 + 8, 144);  // corridor A-F
  // G: north-east gallery joining the great hall and the library.
  Carve(B, 960 - 64, 384 - 8, 0, 960 + 64, 896 + 8, 144);
  Carve(B, 704, 896, 0, 1216, 1408, 288);
  Carve(B, 256 - 8, 960, 0, 704 + 8, 1088, 144);  // corridor C-G

  // Great hall dais with steps, and pillars.
  Solid(B, 832, -128, 0, 1088, 128, 48);
  Solid(B, 832 - 32, -128, 0, 832, 128, 16);
  Solid(B, 832 - 16, -128, 0, 832, 128, 32);
  Solid(B, 624, -336, 0, 672, -288, 352);
  Solid(B, 624, 288, 0, 672, 336, 352);
  Solid(B, 1248, -336, 0, 1296, -288, 352);
  Solid(B, 1248, 288, 0, 1296, 336, 352);

  // Start room: a low wall and two crates.
  Solid(B, -32, 96, 0, 32, 112, 40);
  Solid(B, 160, -224, 0, 224, -160, 48);
  Solid(B, 192, -160, 0, 240, -112, 24);

  // Library: bookcases.
  Solid(B, -240, 800, 0, -208, 1248, 112);
  Solid(B, 208, 800, 0, 240, 1248, 112);
  Solid(B, -96, 960, 0, 96, 992, 56);
  // Armoury: racks.
  Solid(B, -176, -816, 0, -144, -576, 96);
  Solid(B, 144, -816, 0, 176, -576, 96);
}

struct LightDef {
  FLOAT X, Y, Z;
  BYTE Brightness, Hue, Sat, Radius;
};

void AddLights(ULevel* Level) {
  static const LightDef Lights[] = {
      {0, 0, 192, 135, 20, 140, 21},        // A
      {-190, 190, 120, 105, 8, 80, 14},    {190, -190, 120, 105, 8, 80, 14},
      {360, 0, 100, 105, 30, 120, 14},     // corridor A-B
      {960, 0, 300, 150, 25, 150, 28},    // B
      {700, 0, 150, 90, 10, 120, 16},     {1240, 0, 150, 90, 10, 120, 16},
      {0, 520, 100, 105, 30, 120, 14},     // corridor A-C
      {0, 1024, 190, 135, 40, 130, 24},    // C library
      {-800, 0, 200, 135, 160, 140, 24},   // E (cold)
      {-800, 0, -60, 105, 150, 200, 19},   {-420, 0, 100, 105, 30, 120, 14},
      {0, -608, 160, 135, 0, 150, 21},     // F (red)
      {0, -320, 100, 90, 30, 120, 12},    // corridor A-F
      {960, 640, 100, 90, 30, 120, 14},   // gallery
      {960, 1150, 220, 135, 30, 110, 26},  // G
      {480, 1024, 100, 90, 30, 120, 14},  // corridor C-G
  };
  UClass* LightClass = GObj.LoadClass(AActor::StaticClass, NULL, "Engine.Light", NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  for (INT i = 0; i < (INT)(sizeof(Lights) / sizeof(Lights[0])); ++i) {
    const LightDef& L = Lights[i];
    AActor* A = Level->SpawnActor(LightClass, NAME_None, NULL, NULL, FVector(L.X, L.Y, L.Z), FRotator(0, 0, 0), NULL, 0, 0, 0);
    if (!A) { debugf("map: light %i failed", i); continue; }
    A->bStatic = 1;
    A->bNoDelete = 1;
    A->LightType = LT_Steady;
    A->LightEffect = LE_None;
    A->LightBrightness = L.Brightness;
    A->LightHue = L.Hue;
    A->LightSaturation = L.Sat;
    A->LightRadius = L.Radius;
  }
}

struct Placed {
  const char* Class;
  FLOAT X, Y, Z;
  INT Yaw;
};

// Z is the floor height plus the actor's collision half height.
void Populate(ULevel* Level) {
  static const Placed Actors[] = {
      {"UNreal.Krall", 800, -200, 40, 8192},     {"UNreal.Krall", 1100, 250, 40, 40000},
      {"UNreal.Brute", 960, 0, 60, 32768},       {"UNreal.Krall", -820, 150, 40, 0},
      {"UNreal.Brute", 0, 1000, 60, 49152},      {"UNreal.Krall", -120, -700, 40, 16384},
      {"UNreal.Brute", 900, 1150, 60, 49152},    {"UNreal.Krall", 1100, 1250, 40, 32768},
      {"UNreal.Health", 40, 80, 10, 0},          {"UNreal.Health", -820, -150, -75, 0},
      {"UNreal.Health", 0, -500, 10, 0},         {"UNreal.Health", 960, 1200, 10, 0},
      {"UNreal.FlakCannon", 180, 200, 10, 0},    {"UNreal.Eightball", 950, 120, 60, 0},
      {"UNreal.ASMD", -100, -700, 10, 0},        {"UNreal.ShellBox", -150, -450, 10, 0},
      {"UNreal.RocketCan", 1200, -340, 10, 0},  {"UNreal.FlakBox", 40, 1200, 10, 0},
  };
  for (INT i = 0; i < (INT)(sizeof(Actors) / sizeof(Actors[0])); ++i) {
    UClass* C = GObj.LoadClass(AActor::StaticClass, NULL, Actors[i].Class, NULL, LOAD_NoWarn | LOAD_KeepImports, NULL);
    if (!C) { debugf("map: cannot load %s", Actors[i].Class); continue; }
    AActor* A = Level->SpawnActor(C, NAME_None, NULL, NULL, FVector(Actors[i].X, Actors[i].Y, Actors[i].Z), FRotator(0, Actors[i].Yaw, 0), NULL, 0, 0, 0);
    debugf("map: %s -> %s", Actors[i].Class, A ? A->GetFullName() : "spawn failed");
    Mem(Actors[i].Class);
  }
}

}  // namespace

// Builds the level into the running game engine's level and puts the player at the start.
void LionBuildMap(UGameEngine* Engine) {
  ULevel* Level = Engine->GLevel;
  UClass* EdClass = GObj.LoadClass(UEngine::StaticClass, NULL, "Editor.EditorEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL);
  UEditorEngine* Ed = ConstructClassObject<UEditorEngine>(EdClass);
  GEditor = Ed;
  Ed->Client = Engine->Client;
  Ed->Render = Engine->Render;
  Ed->Audio = Engine->Audio;
  Ed->Level = Level;
  Ed->TempModel = new UModel(NULL, 1);

  Mem("start");
  lion_census("start");
  Builder B;
  B.Ed = Ed;
  B.Level = Level;
  B.Brushes = 0;
  B.Tex.Wall = MakeTexture("LionBrick", TK_Brick, 128, 24, 10, 6, 200, 110, 80);
  B.Tex.Floor = MakeTexture("LionStone", TK_Stone, 128, 20, 20, 24, 170, 168, 160);
  B.Tex.Ceil = MakeTexture("LionPanel", TK_Panel, 128, 16, 20, 28, 120, 140, 165);
  B.Tex.Trim = MakeTexture("LionTrim", TK_Trim, 128, 40, 30, 18, 210, 180, 120);
  Ed->CurrentTexture = B.Tex.Wall;

  // The client drops the level's brushes when it loads a map. Building one needs editor rules (static actors
  // can be spawned and destroyed) and the builder brush in the actor list's slot 1.
  GIsEditor = 1;
  if (!Level->Actors(1)) {
    ABrush* Builder = (ABrush*)Level->SpawnActor(ABrush::StaticClass);
    INT At = 0;
    for (INT i = 2; i < Level->Num(); ++i)
      if (Level->Actors(i) == Builder) At = i;
    if (At) Level->Actors(At) = NULL;
    Level->Actors(1) = Builder;
    Builder->Brush = new (Level->GetParent(), NAME_None) UModel(Builder, 1);
    Builder->Brush->Polys = new (Level->GetParent(), NAME_None) UPolys;
  }

  // The stock Entry level has one additive brush (the cube) besides the builder brush: drop its polys.
  for (FStaticBrushIterator It(Level); It; ++It)
    if (*It != Level->Brush() && (*It)->Brush && (*It)->Brush->Polys) (*It)->Brush->Polys->Empty();

  Mem("textures");
  // Ambient light of the level's (only) zone: what meshes get besides the lights.
  Level->GetLevelInfo()->AmbientBrightness = 70;
  Level->GetLevelInfo()->AmbientSaturation = 255;
  const DOUBLE T0 = appSeconds();
  BuildRooms(B);
  debugf("map: %i brushes, csg %i polys", B.Brushes, Level->Model->Surfs->Num());
  Ed->csgRebuild(Level);
  debugf("map: bsp %i nodes, %i surfs, %i zones (%i s)", Level->Model->Nodes->Num(), Level->Model->Surfs->Num(), Level->Model->Nodes->NumZones, (INT)(appSeconds() - T0));
  Mem("bsp");
  AddLights(Level);
  Ed->shadowIlluminateBsp(Level, 0);
  debugf("map: lit %i surfaces, %i light bytes (%i s)", Level->Model->LightMap.Num(), Level->Model->LightBits.Num(), (INT)(appSeconds() - T0));

  Mem("lit");
  lion_census("lit");
  // Player start (a static actor) and the player.
  Level->SpawnActor(APlayerStart::StaticClass, NAME_None, NULL, NULL, FVector(0, 0, 40), FRotator(0, 0, 0), NULL, 0, 0, 0);
  // The editor cameras of the stock map and the temporary one of the lighting pass point at dead viewports.
  for (INT i = 0; i < Level->Num(); ++i) {
    AActor* A = Level->Actors(i);
    if (A && !appStricmp(A->GetClass()->GetName(), "Camera")) {
      ((APlayerPawn*)A)->Player = NULL;
      Level->DestroyActor(A);
    }
  }
  GIsEditor = 0;
  for (INT i = 0; i < Engine->Client->Viewports.Num(); ++i) {
    APlayerPawn* P = Engine->Client->Viewports(i)->Actor;
    if (P) Level->FarMoveActor(P, FVector(0, 0, 40), 1, 1);
  }
  Populate(Level);
  Mem("populated");
  // Give back what the build left behind: unused scratch chunks and unreferenced objects.
  GMem.Exit();  // frees the chunks every FMemStack has returned (the list is shared)
  for (INT i = 0; i < Engine->Client->Viewports.Num(); ++i) {
    UViewport* V = Engine->Client->Viewports(i);
    debugf("map: viewport %i %s actor %s player %s", i, V->GetFullName(), V->Actor ? V->Actor->GetFullName() : "-",
           V->Actor && V->Actor->Player ? V->Actor->Player->GetFullName() : "-");
  }
  GObj.CollectGarbage(GSystem, RF_Intrinsic);
  debugf("map: viewports after gc: %i", Engine->Client->Viewports.Num());
  Mem("collected");
  lion_census("final");
}
