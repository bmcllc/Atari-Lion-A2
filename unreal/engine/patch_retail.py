#!/usr/bin/env python3
"""Reads the packages of the retail game (version-61 files written by Unreal 1.x) with the patched Engine copy.
The Engine in the upstream checkout reads a later layout under the same file version number, so every place where
the two differ is switched at run time by GLionRetail ([Lion] Retail=1 in Lion.ini, defined in engine/lionboot.cpp).
Idempotent: edits are skipped once their marker is in the file.
Usage: patch_retail.py <engine work dir containing Inc/ and Src/>"""
import sys, pathlib

root = pathlib.Path(sys.argv[1])

def edit(path, marker, old, new):
    f = root / path
    text = f.read_text(encoding="latin-1")
    if marker in text:
        return
    if text.count(old) != 1:
        sys.exit(f"patch_retail: {path}: anchor for {marker} found {text.count(old)} times")
    f.write_text(text.replace(old, new), encoding="latin-1")

# UPrimitive: the retail bounding sphere is only its centre (no radius).
edit("Src/UnPrim.cpp", "GLionRetail",
     "void UPrimitive::Serialize( FArchive& Ar )",
     "extern INT GLionRetail;\nvoid UPrimitive::Serialize( FArchive& Ar )")
edit("Src/UnPrim.cpp", "LionRetailSphere",
     "\telse Ar << BoundingBox << BoundingSphere;",
     "\telse if( GLionRetail && Ar.Ver()<=61 ) //LionRetailSphere\n"
     "\t{\n\t\tAr << BoundingBox;\n\t\tFVector C;\n\t\tAr << C;\n"
     "\t\tBoundingSphere = FSphere( C, BoundingBox.IsValid ? (BoundingBox.Max-BoundingBox.Min).Size()*0.5f : 0.f );\n\t}\n"
     "\telse Ar << BoundingBox << BoundingSphere;")

# UMesh serializes the bounds a second time between its arrays, with the same sphere difference.
edit("Src/UnMesh.cpp", "GLionRetail",
     "void UMesh::Serialize( FArchive& Ar )",
     "extern INT GLionRetail;\nvoid UMesh::Serialize( FArchive& Ar )")
edit("Src/UnMesh.cpp", "LionRetailMeshSphere",
     "\tAr << Connects << BoundingBox << BoundingSphere << VertLinks << Textures;",
     "\tif( GLionRetail && Ar.Ver()<=61 ) //LionRetailMeshSphere\n"
     "\t{\n\t\tAr << Connects << BoundingBox;\n\t\tFVector C;\n\t\tAr << C;\n"
     "\t\tBoundingSphere = FSphere( C, BoundingBox.IsValid ? (BoundingBox.Max-BoundingBox.Min).Size()*0.5f : 0.f );\n"
     "\t\tAr << VertLinks << Textures;\n\t}\n"
     "\telse Ar << Connects << BoundingBox << BoundingSphere << VertLinks << Textures;")

# Memory: the heap is 22 MB. Databases are stored with their editor slack (DbMax) and every brush carries its editor
# polygons (an FPoly is ~0.7 KB in memory): keep exactly DbNum entries and drop the polygons, which nothing at run
# time reads (rendering and collision use the Bsp nodes).
edit("Src/UnModel.cpp", "GLionRetailDb",
     "\tif( Ar.IsLoading() )\n\t\tRealloc();\n\tSerializeData( Ar );",
     "\textern INT GLionRetail; //GLionRetailDb\n"
     "\tif( GLionRetail && Ar.IsLoading() )\n"
     "\t{\n\t\tif( IsA(UPolys::StaticClass) ) DbNum = 0;\n\t\tDbMax = DbNum;\n\t}\n"
     "\tif( Ar.IsLoading() )\n\t\tRealloc();\n\tSerializeData( Ar );")

# Memory: the moving-brush tracker doubles every Bsp database (plus 16384 points/vectors) for the brushes of movers.
edit("Src/UnDynBsp.cpp", "GLionRetailExpand",
     "\tRes->SetMax( Max( Res->GetMax(), 2*Res->GetNum() + Slack) );",
     "\textern INT GLionRetail; //GLionRetailExpand\n"
     "\tif( GLionRetail ) Res->SetMax( Max( Res->GetMax(), Res->GetNum() + 2*Min(Slack,2048) ) );\n\telse\n"
     "\tRes->SetMax( Max( Res->GetMax(), 2*Res->GetNum() + Slack) );")

# The bounding spheres inside arrays (per animation frame) lose their radius the same way: read the centre and
# rebuild the radius from the matching box.
edit("Inc/UnMath.h", "GLionRetailSphere",
     "operator<<( FArchive &Ar, FSphere &P )\n\t{\n\t\treturn Ar << (FVector&)P << P.W;",
     "operator<<( FArchive &Ar, FSphere &P )\n\t{\n\t\textern INT GLionRetail; //GLionRetailSphere\n"
     "\t\tif( GLionRetail && Ar.IsLoading() && Ar.Ver()<=61 ) { P.W = 0; return Ar << (FVector&)P; }\n"
     "\t\treturn Ar << (FVector&)P << P.W;")
edit("Src/UnMesh.cpp", "LionRetailFrameSpheres",
     "\tAr << BoundingBoxes << BoundingSpheres;",
     "\tAr << BoundingBoxes << BoundingSpheres;\n"
     "\tif( GLionRetail && Ar.IsLoading() && Ar.Ver()<=61 ) //LionRetailFrameSpheres\n"
     "\t\tfor( INT i=0; i<BoundingSpheres.Num() && i<BoundingBoxes.Num(); i++ )\n"
     "\t\t\tBoundingSpheres(i).W = (BoundingBoxes(i).Max-BoundingBoxes(i).Min).Size()*0.5f;")

# Memory: once a level is up, the CD preload (pinned chunks and staging, ~3 MB) goes back to the heap.
edit("Src/UnDynBsp.cpp", "lion_cd_release_preload();\n#if",
     "FMovingBrushTracker::FMovingBrushTracker( ULevel* ThisLevel )",
     "extern \"C\" void lion_cd_release_preload();\nextern INT GLionRetail;\n#if 1\n#endif\nFMovingBrushTracker::FMovingBrushTracker( ULevel* ThisLevel )")
edit("Src/UnDynBsp.cpp", "//lion_cd_release_call",
     "\tdebugf( NAME_Init, \"Initialized moving brush tracker for %s\", Level->GetFullName() );",
     "\tdebugf( NAME_Init, \"Initialized moving brush tracker for %s\", Level->GetFullName() );\n"
     "\tif( GLionRetail && appStricmp( Level->GetParent()->GetName(), \"Entry\" ) ) lion_cd_release_preload(); //lion_cd_release_call")
