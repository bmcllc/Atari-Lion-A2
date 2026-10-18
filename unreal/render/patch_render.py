#!/usr/bin/env python3
"""Patches the scratch copy of the Render sources that build.sh compiles (the checkout is never modified).
usage: patch_render.py <render work dir>

The renderer sizes two tables for the largest levels it could ever see (128000 points, 65536 nodes: 1.5 MB). Here
they start small and grow to what the level being drawn needs."""
import pathlib, sys

path = pathlib.Path(sys.argv[1]) / "Src" / "UnRender.cpp"
t = path.read_text(encoding="latin-1")

def sub(old, new):
    global t
    if old not in t:
        sys.exit(f"patch_render: pattern not found: {old[:60]!r}")
    t = t.replace(old, new, 1)

sub("URender::FDynamicsCache*			URender::DynamicsCache;",
    "URender::FDynamicsCache*			URender::DynamicsCache;\nstatic INT GPointCap, GNodeCap;")
sub("\tPointCache		= new FStampedPoint [MAX_POINTS];\n\tDynamicsCache   = new FDynamicsCache[MAX_NODES];\n\tappMemset( DynamicsCache, 0, MAX_NODES * sizeof(FDynamicsCache) );",
    "\tGPointCap = GNodeCap = 1024;\n\tPointCache		= new FStampedPoint [GPointCap];\n\tDynamicsCache   = new FDynamicsCache[GNodeCap];\n\tappMemset( DynamicsCache, 0, GNodeCap * sizeof(FDynamicsCache) );")
sub("\tfor( INT i=0; i<MAX_POINTS;  i++ )\n\t\tPointCache [i].Stamp = Stamp;", "\tfor( INT i=0; i<GPointCap;  i++ )\n\t\tPointCache [i].Stamp = Stamp;")
sub("\tcheck(Frame->Level->Model->Nodes->Max()<MAX_NODES);\n\tcheck(Frame->Level->Model->Points->Max()<MAX_POINTS);",
    "\tcheck(Frame->Level->Model->Nodes->Max()<=GNodeCap);\n\tcheck(Frame->Level->Model->Points->Max()<=GPointCap);")
sub("\tNumDynLightSurfs  = 0;\n\tNumDynLightLeaves = 0;\n\tNumPostDynamics   = 0;",
    """\tif( Model->Points->Max() > GPointCap )
	{
		GPointCap = Model->Points->Max() + 1024;
		delete PointCache;
		PointCache = new FStampedPoint [GPointCap];
		for( INT i=0; i<GPointCap; i++ )
			PointCache[i].Stamp = Stamp;
	}
	if( Model->Nodes->Max() > GNodeCap )
	{
		GNodeCap = Model->Nodes->Max() + 1024;
		delete DynamicsCache;
		DynamicsCache = new FDynamicsCache[GNodeCap];
		appMemset( DynamicsCache, 0, GNodeCap * sizeof(FDynamicsCache) );
	}
	NumDynLightSurfs  = 0;
	NumDynLightLeaves = 0;
	NumPostDynamics   = 0;""")
path.write_text(t, encoding="latin-1")
print("patch_render: ok")
