// The slice of the level editor that builds a map on the device: UEditorEngine's CSG, BSP, visibility and
// lighting (UnBsp/UnEdCsg/UnVisi/UnShadow, compiled unchanged) with no windows, undo buffer or UI. Everything
// else of the editor class is stubbed out so that its vtable links.
#include "EditorPrivate.h"

IMPLEMENT_CLASS(UEditorEngine);
EDITOR_API UEditorEngine* GEditor;

FEditorConstraints::FEditorConstraints() : GridEnabled(0), SnapVertices(0), SnapDistance(0), GridSize(16, 16, 16), RotGridEnabled(0), RotGridSize(0, 0, 0) {}
void FEditorConstraints::Snap(FVector&, FVector) {}
void FEditorConstraints::Snap(FRotator&) {}
UBOOL FEditorConstraints::Snap(ULevel*, FVector&, FVector, FRotator&) { return 0; }

UEditorEngine::UEditorEngine() { GEditor = this; }
// The editor object borrows the game engine's client, renderer and audio: destroying it must not shut them down.
void UEditorEngine::Destroy() {
  Client = NULL;
  Render = NULL;
  Audio = NULL;
  GEditor = NULL;
  UObject::Destroy();
}
void UEditorEngine::Serialize(FArchive& Ar) { UEngine::Serialize(Ar); }
void UEditorEngine::NotifyDestroy(void*) {}
void UEditorEngine::NotifyPreChange(void*) {}
void UEditorEngine::NotifyPostChange(void*) {}
void UEditorEngine::NotifyExec(void*, const char*) {}
void UEditorEngine::Init() { UEngine::Init(); GEditor = this; }
UBOOL UEditorEngine::Exec(const char*, FOutputDevice*) { return 0; }
UBOOL UEditorEngine::HookExec(const char*, FOutputDevice*) { return 0; }
int UEditorEngine::Key(UViewport*, EInputKey) { return 0; }
void UEditorEngine::Tick(FLOAT) {}
void UEditorEngine::Draw(UViewport*, BYTE*, INT*) {}
void UEditorEngine::MouseDelta(UViewport*, DWORD, FLOAT, FLOAT) {}
void UEditorEngine::MousePosition(UViewport*, DWORD, FLOAT, FLOAT) {}
void UEditorEngine::Click(UViewport*, DWORD, FLOAT, FLOAT) {}

UBOOL UEditorEngine::SafeExec( const char* Cmd, FOutputDevice* Out ) { return 0; }
void UEditorEngine::Cleanse( FOutputDevice& Out, UBOOL Redraw, const char* TransReset ) {  }
void UEditorEngine::FinishAllSnaps( ULevel* Level ) {  }
void UEditorEngine::RedrawLevel( ULevel* Level ) {  }
void UEditorEngine::ResetSound() {  }
void UEditorEngine::AddActor( ULevel* Level, UClass* Class, FVector V ) {  }
void UEditorEngine::NoteSelectionChange( ULevel* Level ) {  }
void UEditorEngine::NoteActorMovement( ULevel* Level ) {  }
void UEditorEngine::SetPivot( FVector NewPivot, UBOOL SnapPivotToGrid, UBOOL MoveActors ) {  }
void UEditorEngine::ResetPivot() {  }
void UEditorEngine::UpdatePropertiesWindows() {  }
void UEditorEngine::edcamSetMode(INT Mode) {  }
int UEditorEngine::edcamMode(UViewport *Viewport) { return 0; }
void UEditorEngine::SelectNone( ULevel* Level, UBOOL Notify ) {  }
void UEditorEngine::edactSelectAll(ULevel *Level) {  }
void UEditorEngine::edactSelectInside(ULevel *Level) {  }
void UEditorEngine::edactSelectInvert(ULevel *Level) {  }
void UEditorEngine::edactSelectOfClass(ULevel *Level,UClass *Class) {  }
void UEditorEngine::edactDeleteSelected(ULevel *Level) {  }
void UEditorEngine::edactDuplicateSelected(ULevel *Level) {  }
void UEditorEngine::edactCopySelected(ULevel *Level) {  }
void UEditorEngine::edactPasteSelected(ULevel *Level) {  }
void UEditorEngine::edactReplaceSelectedBrush(ULevel *Level) {  }
void UEditorEngine::edactReplaceSelectedWithClass(ULevel *Level,UClass *Class) {  }
void UEditorEngine::edactHideSelected(ULevel *Level) {  }
void UEditorEngine::edactHideUnselected(ULevel *Level) {  }
void UEditorEngine::edactUnHideAll(ULevel *Level) {  }
void UEditorEngine::meshImport( const char* MeshName, UObject* InParent, const char* AnivFname, const char* DataFname, UBOOL Unmirror, UBOOL ZeroTex ) {  }
void UEditorEngine::meshBuildBounds( UMesh* Mesh ) {  }
int UEditorEngine::MakeScripts( UBOOL MakeAll, UBOOL Booting ) { return 0; }
int UEditorEngine::CheckScripts( UClass* Class,FOutputDevice& Out ) { return 0; }
void UEditorEngine::Get( const char* Topic, const char* Item, FOutputDevice& Out ) {  }
void UEditorEngine::Set( const char* Topic, const char* Item, const char* Value ) {  }
void UEditorEngine::EdCallback( DWORD Code, UBOOL Send ) {  }
void UEditorEngine::SetZClipping() {  }
void UEditorEngine::ResetZClipping() {  }
void UEditorEngine::DrawFPoly( struct FSceneNode* Frame, FPoly *Poly, FPlane WireColor, DWORD LineFlags ) {  }
void UEditorEngine::DrawGridSection( struct FSceneNode* Frame, INT ViewportLocX, INT ViewportSXR, INT ViewportGridY, FVector* A, FVector* B, FLOAT* AX, FLOAT* BX, INT AlphaCase ) {  }
void UEditorEngine::DrawWireBackground( struct FSceneNode* Frame ) {  }
void UEditorEngine::DrawLevelBrushes( struct FSceneNode* Frame, UBOOL bStatic, UBOOL bDynamic, UBOOL bActive ) {  }
void UEditorEngine::DrawLevelBrush( struct FSceneNode* Frame, ABrush* Actor, UBOOL bStatic, UBOOL bDynamic, UBOOL bActive ) {  }
void UEditorEngine::DrawBoundingBox( struct FSceneNode* Frame, FBox* Bound, AActor* Actor ) {  }
