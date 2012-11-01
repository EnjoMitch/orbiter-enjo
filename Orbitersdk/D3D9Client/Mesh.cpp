// ==============================================================
// Mesh.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Released under GNU General Public License
// Copyright (C) 2006 Martin Schweiger
//				 2010-2012 Jarmo Nikkanen (D3D9Client implementation)
// ==============================================================

#define VISIBILITY_TOL 0.0015f

#include "Mesh.h"
#include "Log.h"
#include "Scene.h"
#include "D3D9Surface.h"
#include "D3D9Catalog.h"
#include "D3D9Config.h"
#include "DebugControls.h"

using namespace oapi;

static D3DMATERIAL9 defmat = {
	{1,1,1,1},
	{1,1,1,1},
	{0,0,0,1},
	{0,0,0,1},10.0f
};

static D3DMATERIAL9 night_mat = {
	{1,1,1,1},
	{0,0,0,1},
	{0,0,0,1},
	{1,1,1,1},10.0f
};

// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(D3D9Client *client) : D3D9Effect()
{
	_TRACE;
	gc = client;
	Constr = 0;
	sunLight = NULL;
	cAmbient = 0;
	bTemplate = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bGlobalTF = false;
	bModulateMatAlpha = false;
	nGrp = 0;
	nTex = 1;
	Tex = new LPD3D9CLIENTSURFACE[nTex];
	Tex[0] = 0;
	nMtrl = 0;
	MaxFace = 0;
	MaxVert = 0;
	Grp = NULL;
	pVB = NULL;
	pIB = NULL;
	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	MeshCatalog->Add(DWORD(this));
	CheckValidity();
}

// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(D3D9Client *client, MESHHANDLE hMesh, bool asTemplate) : D3D9Effect()
{
	_TRACE;
	gc = client;
	Constr = 1;
	sunLight = NULL;
	cAmbient = 0;
	bTemplate = asTemplate;
	bGlobalTF = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = false;
	MaxFace = 0;
	MaxVert = 0;

	nGrp = oapiMeshGroupCount(hMesh);
	Grp = new GROUPREC*[nGrp];

	memset(Grp, 0, sizeof(GROUPREC*) * nGrp);

	for (DWORD i=0;i<nGrp;i++) {
		Grp[i] = new GROUPREC;	memset(Grp[i], 0, sizeof(GROUPREC));
		MESHGROUPEX *mg = oapiMeshGroupEx(hMesh, i);
		memcpy(Grp[i]->TexIdxEx, mg->TexIdxEx, MAXTEX*sizeof(DWORD));
		memcpy(Grp[i]->TexMixEx, mg->TexMixEx, MAXTEX*sizeof(float));
		Grp[i]->TexIdx  = mg->TexIdx;
		Grp[i]->MtrlIdx = mg->MtrlIdx;
		Grp[i]->FaceOff = MaxFace;
		Grp[i]->VertOff = MaxVert;
		Grp[i]->nFace   = mg->nIdx/3;
		Grp[i]->nVert   = mg->nVtx;
		MaxFace += Grp[i]->nFace;
		MaxVert += Grp[i]->nVert;
	}

	// template meshes are stored in system memory

	D3DPOOL Pool = D3DPOOL_MANAGED;
	DWORD MeshOptions = D3DUSAGE_WRITEONLY;
	if (asTemplate) MeshOptions = 0, Pool = D3DPOOL_SYSTEMMEM;
	
	HRESULT hr1, hr2;

	hr1 = gc->GetDevice()->CreateVertexBuffer(MaxVert*sizeof(NMVERTEX), 0, 0, D3DPOOL_MANAGED, &pVB, NULL);
	hr2 = gc->GetDevice()->CreateIndexBuffer(MaxFace*sizeof(WORD)*3, MeshOptions, D3DFMT_INDEX16, D3DPOOL_MANAGED, &pIB, NULL);

	if (hr1!=S_OK || hr2!=S_OK) {

		LogErr("Failed to create vertex/index buffers. MaxVert=%u, MaxFace=%u, MeshOpt=0x%X, nGrp=%u", MaxVert, MaxFace, MeshOptions, nGrp);

		for (DWORD i=0;i<nGrp;i++) {
			MESHGROUPEX *mg = oapiMeshGroupEx(hMesh, i);
			LogErr("Group[%u] nIdx=%u, nVtx=%u, pIdx=0x%X, pVtx=0x%X", i, mg->nIdx, mg->nVtx, mg->Idx, mg->Vtx); 
		}
		pVB  = NULL; pIB  = NULL;
		return;
	}

	nTex = oapiMeshTextureCount(hMesh)+1;
	Tex = new LPD3D9CLIENTSURFACE[nTex];
	Tex[0] = 0; // 'no texture'

	for (DWORD i=1;i<nTex;i++) Tex[i] = SURFACE(oapiGetTextureHandle(hMesh, i));

	nMtrl = oapiMeshMaterialCount(hMesh);
	if (nMtrl) Mtrl = new D3DMATERIAL9[nMtrl];
	for (DWORD i=0;i<nMtrl;i++)	CopyMaterial(Mtrl+i, oapiMeshMaterial(hMesh, i));

	ProcessInherit();

	for (DWORD i=0;i<nGrp;i++) {
		MESHGROUPEX *mg = oapiMeshGroupEx(hMesh, i);
		CopyGroupEx(Grp[i], mg);
	}

	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	MeshCatalog->Add(DWORD(this));

	UpdateBoundingBox();
	CheckValidity();
}



// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(D3D9Client *client, DWORD groups, const MESHGROUPEX **hGroup, const SURFHANDLE *hSurf) : D3D9Effect()
{
	_TRACE;
	gc = client;
	Constr = 5;
	sunLight = NULL;
	cAmbient = 0;
	bTemplate = false;
	bGlobalTF = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = false;
	MaxFace = 0;
	MaxVert = 0;
	nGrp = groups;

	Grp = new GROUPREC*[nGrp];

	memset(Grp, 0, sizeof(GROUPREC*) * nGrp);

	for (DWORD i=0;i<nGrp;i++) {
		Grp[i] = new GROUPREC;	memset(Grp[i], 0, sizeof(GROUPREC));
		Grp[i]->TexIdxEx[0] = SPEC_DEFAULT;
		Grp[i]->TexMixEx[0] = 0.0f;
		Grp[i]->TexIdx  = i;
		Grp[i]->MtrlIdx = SPEC_DEFAULT;
		Grp[i]->FaceOff = MaxFace;
		Grp[i]->VertOff = MaxVert;
		Grp[i]->nFace   = hGroup[i]->nIdx/3;
		Grp[i]->nVert   = hGroup[i]->nVtx;
		MaxFace += Grp[i]->nFace;
		MaxVert += Grp[i]->nVert;
	}

	HR(gc->GetDevice()->CreateVertexBuffer(MaxVert*sizeof(NMVERTEX), 0, 0, D3DPOOL_MANAGED, &pVB, NULL));
	HR(gc->GetDevice()->CreateIndexBuffer(MaxFace*sizeof(WORD)*3, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &pIB, NULL));

	nMtrl = 0;
	nTex = nGrp+1;
	Tex = new LPD3D9CLIENTSURFACE[nTex];
	Tex[0] = 0; // 'no texture'

	for (DWORD i=1;i<nTex;i++) Tex[i] = SURFACE(hSurf[i-1]);

	ProcessInherit();

	for (DWORD i=0;i<nGrp;i++) CopyGroupEx(Grp[i], hGroup[i]);

	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	MeshCatalog->Add(DWORD(this));

	UpdateBoundingBox();
	CheckValidity();
}



// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(D3D9Client *client, const MESHGROUPEX *pGroup, const MATERIAL *pMat, D3D9ClientSurface *pTex) : D3D9Effect()
{
	_TRACE;
	gc = client;
	Constr = 2;
	sunLight = NULL;
	cAmbient = 0;
	bGlobalTF = false;
	bTemplate = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = false;

	// template meshes are stored in system memory	
	nGrp   = 1;
	Grp    = new GROUPREC*[nGrp];
	Grp[0] = new GROUPREC; memset(Grp[0], 0, sizeof(GROUPREC));
	nTex   = 2;
	Tex	   = new LPD3D9CLIENTSURFACE[nTex];
	Tex[0] = 0; // 'no texture'
	Tex[1] = pTex; 
	nMtrl  = 1;
	Mtrl   = new D3DMATERIAL9[nMtrl];

	memcpy(Grp[0]->TexIdxEx, pGroup->TexIdxEx, MAXTEX*sizeof(DWORD));
	memcpy(Grp[0]->TexMixEx, pGroup->TexMixEx, MAXTEX*sizeof(float));

	Grp[0]->TexIdx  = pGroup->TexIdx;
	Grp[0]->MtrlIdx = pGroup->MtrlIdx;
	Grp[0]->VertOff = 0;
	Grp[0]->FaceOff = 0;
	Grp[0]->nFace   = pGroup->nIdx/3;
	Grp[0]->nVert   = pGroup->nVtx;
	
	MaxFace = Grp[0]->nFace;
	MaxVert = Grp[0]->nVert;

	HR(gc->GetDevice()->CreateVertexBuffer(MaxVert*sizeof(NMVERTEX), 0, 0, D3DPOOL_MANAGED, &pVB, NULL));
	HR(gc->GetDevice()->CreateIndexBuffer(MaxFace*sizeof(WORD)*3, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &pIB, NULL));

	CopyMaterial(&Mtrl[0], pMat);

	CopyGroupEx(Grp[0], pGroup);

	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	MeshCatalog->Add(DWORD(this));

	UpdateBoundingBox();
	CheckValidity();
}

// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(D3D9Client *client, const MESHGROUPEX *pGroup) : D3D9Effect()
{
	_TRACE;
	gc = client;
	Constr = 3;
	sunLight = NULL;
	cAmbient = 0;
	bGlobalTF = false;
	bTemplate = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = false;

	// template meshes are stored in system memory	
	nGrp   = 1;
	Grp    = new GROUPREC*[nGrp];
	Grp[0] = new GROUPREC; memset(Grp[0], 0, sizeof(GROUPREC));
	nTex   = 1;
	Tex	   = new LPD3D9CLIENTSURFACE[nTex];
	Tex[0] = 0; // 'no texture'
	nMtrl  = 1;
	Mtrl   = new D3DMATERIAL9[nMtrl];

	memcpy(Grp[0]->TexIdxEx, pGroup->TexIdxEx, MAXTEX*sizeof(DWORD));
	memcpy(Grp[0]->TexMixEx, pGroup->TexMixEx, MAXTEX*sizeof(float));
	Grp[0]->TexIdx  = pGroup->TexIdx;
	Grp[0]->MtrlIdx = pGroup->MtrlIdx;
	Grp[0]->VertOff = 0;
	Grp[0]->FaceOff = 0;
	Grp[0]->nFace   = pGroup->nIdx/3;
	Grp[0]->nVert   = pGroup->nVtx;

	MaxFace = Grp[0]->nFace;
	MaxVert = Grp[0]->nVert;

	HR(gc->GetDevice()->CreateVertexBuffer(MaxVert*sizeof(NMVERTEX), 0, 0, D3DPOOL_MANAGED, &pVB, NULL));
	HR(gc->GetDevice()->CreateIndexBuffer(MaxFace*sizeof(WORD)*3, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &pIB, NULL));

	CopyGroupEx(Grp[0], pGroup);
	CopyMaterial(&Mtrl[0], (const MATERIAL *)&defmat);

	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	MeshCatalog->Add(DWORD(this));

	UpdateBoundingBox();
	CheckValidity();
}

// ===========================================================================================
//
D3D9Mesh::D3D9Mesh(const D3D9Mesh &mesh) : D3D9Effect()
{
	_TRACE;
	// note: 'mesh' must be a template mesh, because we may not be able to
	// access vertex data in video memory
	gc = mesh.gc;
	Constr = 4;
	sunLight = NULL;
	cAmbient = 0;
	bGlobalTF = false;
	bTemplate = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = mesh.bModulateMatAlpha;

	nGrp = mesh.nGrp;
	Grp = new GROUPREC*[nGrp];

	MaxFace = mesh.MaxFace;
	MaxVert = mesh.MaxVert;

	for (DWORD i=0;i<nGrp;i++) {
		Grp[i] = new GROUPREC; 
		memcpy(Grp[i], mesh.Grp[i], sizeof(GROUPREC));
	}

	HR(gc->GetDevice()->CreateVertexBuffer(MaxVert*sizeof(NMVERTEX), 0, 0, D3DPOOL_MANAGED, &pVB, NULL));
	HR(gc->GetDevice()->CreateIndexBuffer(MaxFace*sizeof(WORD)*3, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &pIB, NULL));

	// ----------------------------------------------------------------

	WORD *pISrc,*pITgt;
	NTVERTEX *pVSrc, *pVTgt;

	HR(mesh.pIB->Lock(0, 0, (LPVOID*)&pISrc, 0)); 
	HR(pIB->Lock(0, 0, (LPVOID*)&pITgt, 0)); 
	memcpy(pITgt, pISrc, MaxFace*6); 
	HR(mesh.pIB->Unlock());
	HR(pIB->Unlock());


	HR(mesh.pVB->Lock(0, 0, (LPVOID*)&pVSrc, 0)); 
	HR(pVB->Lock(0, 0, (LPVOID*)&pVTgt, 0)); 
	memcpy(pVTgt, pVSrc, MaxVert*sizeof(NMVERTEX)); 
	HR(mesh.pVB->Unlock());
	HR(pVB->Unlock());

	// ----------------------------------------------------------------

	nTex = mesh.nTex;
	Tex = new LPD3D9CLIENTSURFACE[nTex];

	for (DWORD i=0;i<nTex;i++) Tex[i] = mesh.Tex[i];
			
	nMtrl = mesh.nMtrl;
	if (nMtrl) Mtrl = new D3DMATERIAL9[nMtrl];
	memcpy (Mtrl, mesh.Mtrl, nMtrl*sizeof(D3DMATERIAL9));

	// ATTENTION:  Do we need to copy transformations
	mTransform = mesh.mTransform;
	mTransformInv = mesh.mTransformInv;
	bGlobalTF = mesh.bGlobalTF;

	MeshCatalog->Add(DWORD(this));
	UpdateBoundingBox();
	CheckValidity();
}

// ===========================================================================================
//
void D3D9Mesh::DeleteGroup(GROUPREC *grp)
{
	delete grp;
}

// ===========================================================================================
//
D3D9Mesh::~D3D9Mesh()
{
	_TRACE;
	if (!pVB) return;

	if (MeshCatalog->Remove(DWORD(this))) LogAlw("Mesh 0x%X Removed from catalog",this);
	else 								  LogErr("Mesh 0x%X wasn't in meshcatalog",this);
	
	if (nGrp && Grp) for (DWORD g=0;g<nGrp;g++) delete Grp[g];
	
	if (Grp) delete []Grp; 
	if (nTex) delete []Tex;
	if (nMtrl) delete []Mtrl;
	if (pIB) pIB->Release();
	if (pVB) pVB->Release();

	pIB = NULL;
	pVB = NULL;

	LogOk("Mesh 0x%X Deleted successfully -------------------------------",this);
}


// ===========================================================================================
//
void D3D9Mesh::CheckValidity()
{

	if (Constr!=5) {
		float lim = 5e3;
		for (DWORD i=0;i<nGrp;i++) {
			D3DXVECTOR3 s = Grp[i]->BBox.max - Grp[i]->BBox.min;
			if (fabs(s.x)>lim || fabs(s.y)>lim || fabs(s.z)>lim) {
				LogWrn("D3D9Mesh(0x%X) Has a large group (%0.0fm x %0.0fm x %0.0fm) idx=%u/%u nVert=%u, nFace=%u", this, s.x, s.y, s.z, i, nGrp-1, Grp[i]->nVert, Grp[i]->nFace);
			}
		}
	}

	for (DWORD i=0;i<nGrp;i++) {
		if (Grp[i]->nVert==0) LogWrn("MeshGroup has no vertices");
		if (Grp[i]->nFace==0) LogWrn("MeshGroup has no faces");
		if (Grp[i]->BBox.bs.w<1e-3) LogWrn("Small Bounding Sphere rad=%g nVtx=%u", Grp[i]->BBox.bs.w, Grp[i]->nVert);
	}
}




// ===========================================================================================
//
void D3D9Mesh::ProcessInherit()
{
	if (!pVB) return;
	if (Grp[0]->MtrlIdx == SPEC_INHERIT) Grp[0]->MtrlIdx = SPEC_DEFAULT; 
	if (Grp[0]->TexIdx == SPEC_INHERIT) Grp[0]->TexIdx = SPEC_DEFAULT;
	if (Grp[0]->TexIdxEx[0] == SPEC_INHERIT) Grp[0]->TexIdxEx[0] = SPEC_DEFAULT;

	bool bPopUp = false;

	for (DWORD i=0;i<nGrp;i++) {
	
		if (Grp[i]->UsrFlag & 0x8) LogErr("Flag 0x8 in use");
		
		// Inherit Material
		if (Grp[i]->MtrlIdx == SPEC_INHERIT) Grp[i]->MtrlIdx = Grp[i-1]->MtrlIdx;
			
		// Inherit Texture
		if (Grp[i]->TexIdx == SPEC_DEFAULT) Grp[i]->TexIdx = 0;
		else if (Grp[i]->TexIdx == SPEC_INHERIT) Grp[i]->TexIdx = Grp[i-1]->TexIdx;
		else Grp[i]->TexIdx++;

		// Inherit Night Texture
		if (Grp[i]->TexIdxEx[0] == SPEC_DEFAULT) Grp[i]->TexIdxEx[0] = 0;
		else if (Grp[i]->TexIdxEx[0] == SPEC_INHERIT) Grp[i]->TexIdxEx[0] = Grp[i-1]->TexIdxEx[0];
		else Grp[i]->TexIdxEx[0]++;
	
		// Do some safety checks
		if (Grp[i]->TexIdx>=nTex) {
			LogErr("Mesh(0x%X) has a texture index %u in group %u out of range. Constr=%u", this, Grp[i]->TexIdx, i, Constr); 
			Grp[i]->TexIdx = 0;
			bPopUp = true;
		}
		if (Grp[i]->TexIdxEx[0]>=nTex) {
			LogErr("Mesh(0x%X) has a night texture index %u in group %u out of range. Constr=%u", this, Grp[i]->TexIdxEx[0], i, Constr); 
			Grp[i]->TexIdxEx[0] = 0;
			bPopUp = true;
		}

		if (Grp[i]->MtrlIdx!=SPEC_DEFAULT) {
			if (Grp[i]->MtrlIdx>=nMtrl) {
				LogErr("Mesh(0x%X) has a material index %u in group %u out of range. Constr=%u", this, Grp[i]->MtrlIdx, i, Constr); 
				Grp[i]->MtrlIdx = SPEC_DEFAULT;
				bPopUp = true;
			}
		}
	}
	if (bPopUp) MessageBoxA(NULL, "Invalid Mesh Detected", "D3D9Client Error:",MB_OK);
}

// ===========================================================================================
//
D3DXVECTOR3 D3D9Mesh::GetGroupSize(DWORD idx)
{
	if (idx>=nGrp) return D3DXVECTOR3(0,0,0);
	if (Grp[idx]->nVert<2) return D3DXVECTOR3(0,0,0);
	return D3DXVECTOR3f4(Grp[idx]->BBox.max - Grp[idx]->BBox.min);
}

// ===========================================================================================
//
void D3D9Mesh::ResetTransformations()
{
	D3DXMatrixIdentity(&mTransform);
	D3DXMatrixIdentity(&mTransformInv);
	bGlobalTF = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	for (DWORD i=0;i<nGrp;i++) {
		D3DXMatrixIdentity(&Grp[i]->Transform);
		D3DXMatrixIdentity(&Grp[i]->TransformInv);
		Grp[i]->bTransform = false;
	}
}

// ===========================================================================================
// 
void D3D9Mesh::UpdateTangentSpace(NMVERTEX *pVrt, WORD *pIdx, DWORD nVtx, DWORD nFace, bool bTextured)
{
	if (bTextured) {

		D3DXVECTOR3 *ta = new D3DXVECTOR3[nVtx];
		D3DXVECTOR3 *bi = new D3DXVECTOR3[nVtx];

		for (DWORD i=0;i<nVtx;i++) bi[i]=ta[i]=D3DXVECTOR3(0,0,0);

		for (DWORD i=0;i<nFace;i++) {

			DWORD i0 = pIdx[i*3];
			DWORD i1 = pIdx[i*3+1];
			DWORD i2 = pIdx[i*3+2];

			D3DXVECTOR3 r0 = D3DXVECTOR3(pVrt[i0].x,  pVrt[i0].y,  pVrt[i0].z); 
			D3DXVECTOR3 r1 = D3DXVECTOR3(pVrt[i1].x,  pVrt[i1].y,  pVrt[i1].z);
			D3DXVECTOR3 r2 = D3DXVECTOR3(pVrt[i2].x,  pVrt[i2].y,  pVrt[i2].z);
			D3DXVECTOR2 t0 = D3DXVECTOR2(pVrt[i0].u, pVrt[i0].v);  
			D3DXVECTOR2 t1 = D3DXVECTOR2(pVrt[i1].u, pVrt[i1].v);   
			D3DXVECTOR2 t2 = D3DXVECTOR2(pVrt[i2].u, pVrt[i2].v); 
			
			float u0 = t1.x - t0.x; 
			float v0 = t1.y - t0.y; 
			float u1 = t2.x - t0.x;   
			float v1 = t2.y - t0.y;

			D3DXVECTOR3 k0 = r1 - r0;
			D3DXVECTOR3 k1 = r2 - r0;
		
			float q = (u0*v1-u1*v0);
			if (q==0) q = 1.0f;
			else q = 1.0f / q;

			D3DXVECTOR3 t = ((k0*v1 - k1*v0) * q);		
			D3DXVECTOR3 b = ((k1*u0 - k0*u1) * q); 

			ta[i0]+=t; ta[i1]+=t; ta[i2]+=t;
			bi[i0]+=b; bi[i1]+=b; bi[i2]+=b;
		}

		for (DWORD i=0;i<nVtx; i++) {

			D3DXVECTOR3 n = D3DXVECTOR3(pVrt[i].nx,  pVrt[i].ny,  pVrt[i].nz);
			D3DXVec3Normalize(&n, &n);

			D3DXVECTOR3 t = (ta[i] - n * D3DXVec3Dot(&ta[i],&n));
			D3DXVECTOR3 b = (bi[i] - n * D3DXVec3Dot(&bi[i],&n));

			D3DXVec3Normalize(&t, &t);
			//D3DXVec3Cross(&b,&n,&t);
			D3DXVec3Normalize(&b, &b);

			pVrt[i].tx = t.x;
			pVrt[i].ty = t.y;
			pVrt[i].tz = t.z;

			pVrt[i].bx = b.x;
			pVrt[i].by = b.y;
			pVrt[i].bz = b.z;
		}

		delete []ta;
		delete []bi;
	}
	else {
		for (DWORD i=0;i<nVtx; i++) {

			D3DXVECTOR3 n = D3DXVECTOR3(pVrt[i].nx,  pVrt[i].ny,  pVrt[i].nz);
			D3DXVECTOR3 t = Perpendicular(&n);
			D3DXVECTOR3 b;

			D3DXVec3Normalize(&n, &n);
			D3DXVec3Normalize(&t, &t);
			D3DXVec3Cross(&b,&n,&t);
			D3DXVec3Normalize(&b, &b);

			pVrt[i].tx = t.x;
			pVrt[i].ty = t.y;
			pVrt[i].tz = t.z;

			pVrt[i].bx = b.x;
			pVrt[i].by = b.y;
			pVrt[i].bz = b.z;
		}
	}
}


// ===========================================================================================
// 
bool D3D9Mesh::CopyGroupEx(GROUPREC *grp, const MESHGROUPEX *mg)
{
	if (!pVB) return false;
	if (mg->nVtx>65535) LogErr("Mesh group vertex count is greater than 65535");
	
	grp->UsrFlag = mg->UsrFlag;
	grp->IntFlag = mg->Flags;
	
	//LogErr("IntFlags = 0x%hX,  UsrFlag=0x%X",grp->IntFlag,grp->UsrFlag);

	D3DXMatrixIdentity(&grp->Transform);

	WORD *pIndex;
	NMVERTEX *pVert;
	NTVERTEX *pNT = mg->Vtx;

	HR(pIB->Lock(grp->FaceOff*6, grp->nFace*6, (LPVOID*)&pIndex, 0)); 
	HR(pVB->Lock(grp->VertOff*sizeof(NMVERTEX), grp->nVert*sizeof(NMVERTEX), (LPVOID*)&pVert, 0)); 

	memcpy(pIndex, mg->Idx, mg->nIdx*sizeof(WORD)); 

	for (DWORD i=0;i<mg->nVtx; i++) {
		float x = pNT[i].nx; float y = pNT[i].ny; float z = pNT[i].nz;
		float b = 1.0f/sqrt(y*y+z*z+x*x);
		pVert[i].nx = (x*b);
		pVert[i].ny = (y*b);
		pVert[i].nz = (z*b);
		pVert[i].x  = pNT[i].x;
		pVert[i].y  = pNT[i].y;
		pVert[i].z  = pNT[i].z;
		pVert[i].u  = pNT[i].tu;
		pVert[i].v  = pNT[i].tv;
	}

	if (Config->UseNormalMap) UpdateTangentSpace(pVert, pIndex, mg->nVtx, mg->nIdx/3, grp->TexIdx!=0);

	if (mg->nVtx>0) BoundingBox(pVert, mg->nVtx, &grp->BBox);
	else D9ZeroAABB(&grp->BBox);
	
	HR(pIB->Unlock());
	HR(pVB->Unlock());

	return true;
}



// ===========================================================================================
// Mesh Update routine for AMSO
//
void D3D9Mesh::UpdateGroupEx(DWORD idx, const MESHGROUPEX *mg)
{
	_TRACE;
	if (!pVB) return;
	GROUPREC *grp = Grp[idx];
	NMVERTEX *pVert = LockVertexBuffer(idx);
	NTVERTEX *pNT = mg->Vtx;

	if (pVert) {	
		for (DWORD i=0;i<mg->nVtx;i++) {
			pVert[i].x = pNT[i].x;
			pVert[i].y = pNT[i].y;
			pVert[i].z = pNT[i].z;	
		}
		if (Config->UseNormalMap) UpdateTangentSpace(pVert, mg->Idx, mg->nVtx, mg->nIdx/3, grp->TexIdx!=0);
		if (mg->nVtx>0) BoundingBox(pVert, mg->nVtx, &grp->BBox);
		else D9ZeroAABB(&grp->BBox);
		UnLockVertexBuffer();
	}
}


// ===========================================================================================
//
bool D3D9Mesh::CopyMaterial(D3DMATERIAL9 *mat9, const MATERIAL *mat)
{
	if (!pVB) return true;
	memcpy (mat9, mat, sizeof (D3DMATERIAL9));
	return true;
}


// ===========================================================================================
// This is required by Client implementation see clbkEditMeshGroup
//
int D3D9Mesh::EditGroup(DWORD grp, GROUPEDITSPEC *ges)
{
	_TRACE;
	if (!pVB) return 1;
	if (grp >= nGrp) return 1;

	bBSRecompute = true;

	GROUPREC *g = Grp[grp];
	DWORD i, vi;
	DWORD flag = ges->flags;

	if (flag & GRPEDIT_SETUSERFLAG)	     g->UsrFlag  = ges->UsrFlag;
	else if (flag & GRPEDIT_ADDUSERFLAG) g->UsrFlag |= ges->UsrFlag;
	else if (flag & GRPEDIT_DELUSERFLAG) g->UsrFlag &= ~ges->UsrFlag;

	if (flag & GRPEDIT_VTX) {
		NMVERTEX *vtx = LockVertexBuffer(grp);
		if (vtx) {
			for (i = 0; i < ges->nVtx; i++) {
				vi = (ges->vIdx ? ges->vIdx[i] : i);
				if (vi < g->nVert) {
					if (flag & GRPEDIT_VTXCRDX) vtx[vi].x = ges->Vtx[i].x;
					if (flag & GRPEDIT_VTXCRDY) vtx[vi].y = ges->Vtx[i].y;
					if (flag & GRPEDIT_VTXCRDZ) vtx[vi].z = ges->Vtx[i].z;
					if (flag & GRPEDIT_VTXNMLX) vtx[vi].nx = ges->Vtx[i].nx;
					if (flag & GRPEDIT_VTXNMLY) vtx[vi].ny = ges->Vtx[i].ny;
					if (flag & GRPEDIT_VTXNMLZ) vtx[vi].nz = ges->Vtx[i].nz;
					if (flag & GRPEDIT_VTXTEXU) vtx[vi].u = ges->Vtx[i].tu;
					if (flag & GRPEDIT_VTXTEXV) vtx[vi].v = ges->Vtx[i].tv;
				}
			}

			if (Config->UseNormalMap) {
				WORD *idx=0;
				if (pIB->Lock(g->FaceOff*6, g->nFace*6, (LPVOID*)&idx, 0)==S_OK) { 
					UpdateTangentSpace(vtx, idx, g->nVert, g->nFace, g->TexIdx!=0); 
					pIB->Unlock();
				}
			}
			
			if (g->nVert>0) BoundingBox(vtx, g->nVert, &g->BBox);
			else D9ZeroAABB(&g->BBox);

			UnLockVertexBuffer();
		}
	}
	return 0;
}


// ===========================================================================================
//
bool D3D9Mesh::SetTexture(DWORD texidx, LPD3D9CLIENTSURFACE tex)
{
	if (!pVB) return false;

	if (texidx >= nTex) {
		LogErr("D3D9Mesh::SetTexture(%u, 0x%X) index out of range",texidx,tex);
		return false;
	}
	if (Tex[texidx]) Tex[texidx]->Release();
	Tex[texidx] = tex;
	tex->IncRef();
	LogBlu("D3D9Mesh(0x%X)::SetTexture(%u, 0x%X) (%s)",this,texidx,tex,SURFACE(tex)->GetName());
	return true;
}


// ===========================================================================================
//
bool D3D9Mesh::HasTexture(SURFHANDLE hSurf)
{
	if (!pVB) return false;
	for (DWORD i=0;i<nTex;i++) if (Tex[i]==hSurf) return true;
	return false;
}

// ===========================================================================================
//
void D3D9Mesh::SetTexMixture(DWORD ntex, float mix)
{
	if (!pVB) return;
	ntex--;
	for (DWORD g = 0; g < nGrp; g++) if (Grp[g]->TexIdxEx[ntex] != SPEC_DEFAULT) Grp[g]->TexMixEx[ntex] = mix;
}

// ===========================================================================================
//
void D3D9Mesh::SetSunLight(D3D9Light *light)
{
	if (!pVB) return;
	sunLight = light;
}


// ===========================================================================================
//
NMVERTEX * D3D9Mesh::LockVertexBuffer(DWORD grp)
{
	_TRACE;
	if (!pVB) return NULL;
	NMVERTEX *pVert;
	bBSRecompute = true;

	if (grp>=nGrp) {
		LogErr("D3D9Mesh(0x%X)::GetVertexBuffer(%u) index out of range",this,grp);
		return NULL;
	}

	if (pVB->Lock(Grp[grp]->VertOff*sizeof(NMVERTEX), Grp[grp]->nVert*sizeof(NMVERTEX), (LPVOID*)&pVert, 0)==S_OK) {
		return pVert;
	}
	else {
		LogErr("D3D9Mesh(0x%X)::GetVertexBuffer(%u)",this,grp);
		return NULL;
	}
}

// ===========================================================================================
//
void D3D9Mesh::UnLockVertexBuffer()
{
	_TRACE;
	if (!pVB) return;
	HR(pVB->Unlock());
}

// ===========================================================================================
//
void D3D9Mesh::UpdateBoundings(DWORD idx)
{
	if (!pVB) return;
	if (idx>=nGrp) {
		LogErr("D3D9Mesh(0x%X)::UpdateBoundings(%u) index out of range",this,idx);
		return;
	}

	bBSRecompute = true;
	NMVERTEX *vtx = LockVertexBuffer(idx);
	GROUPREC *grp = Grp[idx];

	if (vtx && grp->nVert>0) BoundingBox(vtx, grp->nVert, &grp->BBox);
	else D9ZeroAABB(&grp->BBox);

	if (vtx) UnLockVertexBuffer();	
}

// ===========================================================================================
//
D3D9Mesh::GROUPREC *D3D9Mesh::GetGroup(DWORD idx)
{ 
	static int count = 10; 
	if (idx<nGrp) return Grp[idx]; 
	if ((count--)>0) LogErr("Mesh group index out of range. idx=%u nGrp=%u", idx, nGrp);
	return NULL;
}


// ===========================================================================================
//
void D3D9Mesh::SetAmbientColor(D3DCOLOR c)
{
	if (!pVB) return;
	cAmbient = c;
}


// ===========================================================================================
//
void D3D9Mesh::SetupFog(const LPD3DXMATRIX pW)
{
	if (!pVB) return;
	FX->SetVector(eAttennuate, &D3DXVECTOR4(1,1,1,1)); 
	FX->SetVector(eInScatter,  &D3DXVECTOR4(0,0,0,0));
}


// ===========================================================================================
//
void D3D9Mesh::RenderGroup(LPDIRECT3DDEVICE9 dev, const GROUPREC *grp)
{
	if (!pVB) return;
	gc->GetDevice()->SetVertexDeclaration(pMeshVertexDecl);
	gc->GetDevice()->SetStreamSource(0, pVB, 0, sizeof(NMVERTEX));
	gc->GetDevice()->SetIndices(pIB);
	gc->GetDevice()->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, grp->VertOff, 0, grp->nVert, grp->FaceOff*3, grp->nFace);
	gc->GetStats()->Vertices += grp->nVert;
	gc->GetStats()->Draw++;
}


// Used only by ring manager --------------------------------------------------------------------
//
void D3D9Mesh::RenderRings(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW, LPDIRECT3DTEXTURE9 pTex)
{
	if (!pVB) return;

	gc->GetStats()->Vertices += Grp[0]->nVert;
	gc->GetStats()->Meshes++;

	UINT numPasses = 0;
	HR(FX->SetTechnique(eRingTech));
	HR(FX->SetMatrix(eW, pW));
	HR(FX->SetTexture(eTex0, pTex));
	if (sunLight) FX->SetValue(eSun, sunLight, sizeof(D3D9Light));
	HR(FX->SetValue(eMat, &defmat, sizeof(D3DMATERIAL9)));
	HR(FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE));
	HR(FX->BeginPass(0));
	RenderGroup(dev, Grp[0]);
	HR(FX->EndPass());
	HR(FX->End());	
}

// ===========================================================================================
// This is a special rendering routine used by VVessel to render MFD screens and HUD
//
// Used by MFDs, HUD
//
void D3D9Mesh::RenderMeshGroup(LPDIRECT3DDEVICE9 dev, DWORD Tech, DWORD idx, const LPD3DXMATRIX pW, LPD3D9CLIENTSURFACE pTex)
{
	if (!pVB) return;

	if (idx>=nGrp) return;

	GROUPREC *grp = Grp[idx];

	if (DWORD(grp)==0xFDFDFDFD || grp==NULL || pTex==NULL) {
		LogErr("D3D9Mesh::RenderMeshGroup() has a group 0x%X, Tech=%u, pTex=0x%X",grp,Tech,pTex);
		FatalAppExitA(0,"Critical error has occured. See Orbiter.log for details");
	}

	UINT numPasses = 0;
	D3DXMATRIX q;

	if (Tech==0) HR(FX->SetTechnique(eVCHudTech));
	if (Tech==1) HR(FX->SetTechnique(eVCMFDTech));

	if (Grp[idx]->bTransform) {
		if (bGlobalTF)  FX->SetMatrix(eGT, D3DXMatrixMultiply(&q, &mTransform, &Grp[idx]->Transform));
		else FX->SetMatrix(eGT, &Grp[idx]->Transform);
	}
	else FX->SetMatrix(eGT, &mTransform);

	HR(FX->SetMatrix(eW, pW));
	HR(FX->SetTexture(eTex0, pTex->GetTexture()));

	if (sunLight) FX->SetValue(eSun, sunLight, sizeof(D3D9Light));

	// Setup Mesh group material ==============================================================================
	//		
	D3DMATERIAL9 *mat = &defmat;
	if (Grp[idx]->MtrlIdx!=SPEC_DEFAULT) mat = &Mtrl[Grp[idx]->MtrlIdx];
	
	HR(FX->SetValue(eMat, mat, sizeof(D3DMATERIAL9)));
	HR(FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE));
	HR(FX->BeginPass(0));
	RenderGroup(dev, grp);
	HR(FX->EndPass());
	HR(FX->End());	
}



// ================================================================================================
// This is a rendering routine for a Exterior Mesh, non-spherical moons/asteroids
//
void D3D9Mesh::Render(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW, int iTech)
{
	
	DWORD flags=0, selmsh=0, selgrp=0, displ=0; // Debug Variables
	bool bActiveVisual = false;

	_TRACE;
	if (!pVB) return;

	if (iTech==RENDER_VC) {
		RenderVC(dev, pW);
		return;
	}

	if (DebugControls::IsActive()) {
		flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
		selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
		displ  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);
		bActiveVisual = (pCurrentVisual==DebugControls::GetVisual());
		if (displ>0 && !bActiveVisual) return;
		if ((displ==2 || displ==3) && uCurrentMesh!=selmsh) return;
	}

	Scene *scn = gc->GetScene(); 
	
	float mix = 0.0f;
	bool bClamp = false;
	bool bMeshCull = true;
	bool bTextured = true;
	bool bGroupCull = true;

	bool bUseNormalMap = (Config->UseNormalMap==1);

	switch (iTech) {

		case RENDER_BASE:
			bMeshCull = false;
			FX->SetTechnique(eVesselTech);
			break;

		case RENDER_BASEBS:
			bMeshCull = false;
			FX->SetTechnique(eBuildingTech);
			break;

		case RENDER_ASTEROID:
			bMeshCull = false;
			bGroupCull = false;
			FX->SetTechnique(eVesselTech);
			break;

		case RENDER_BASETILES:
			bMeshCull = false;
			FX->SetTechnique(eBaseTile);
			break;

		case RENDER_VESSEL:
			FX->SetTechnique(eVesselTech);
			break;
	}

	D3DXMATRIX mWorldView, mWorldInverse, q, qq;
	D3DXMatrixMultiply(&mWorldView, pW, scn->GetViewMatrix());

	D3DXVECTOR4 Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	if (bMeshCull) if (!D9IsAABBVisible(&BBox, &mWorldView, &Field)) return;
	
	gc->GetStats()->Meshes++;

	if (bUseNormalMap) {
		D3DXMatrixInverse(&mWorldInverse, NULL, pW); // TODO: Inverse exists already
		FX->SetMatrix(eWI, &mWorldInverse);
	}

	D3DMATERIAL9 *mat, *old_mat = NULL;
	LPD3D9CLIENTSURFACE old_tex = NULL;
	LPD3D9CLIENTSURFACE Diffuse = NULL;
	LPDIRECT3DTEXTURE9  pNorm = NULL;
	LPDIRECT3DTEXTURE9  pSpec = NULL;
	LPDIRECT3DTEXTURE9  pEmis = NULL;

	dev->SetVertexDeclaration(pMeshVertexDecl);
	dev->SetStreamSource(0, pVB, 0, sizeof(NMVERTEX));
	dev->SetIndices(pIB);

	if (flags&DBG_FLAGS_DUALSIDED) dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	FX->SetMatrix(eW, pW);
	FX->SetBool(eModAlpha, bModulateMatAlpha);
	FX->SetValue(eSun, sunLight, sizeof(D3D9Light));
	FX->SetBool(eNight, false);
	FX->SetBool(eUseSpec, false);
	FX->SetBool(eUseEmis, false);
	FX->SetBool(eDebugHL, false);
	
	int nLights = gc->GetScene()->GetLightCount();
	const D3D9Light *pLights = gc->GetScene()->GetLights();

	if (pLights && nLights>0 && iTech==0) { 
		FX->SetValue(eLights, pLights, 12*sizeof(D3D9Light));
		FX->SetInt(eLightCount, nLights);
	}
	else {
		FX->SetInt(eLightCount, 0);
	}

	UINT numPasses = 0;
	HR(FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE));

	if (iTech==RENDER_VESSEL || iTech==RENDER_BASE || iTech==RENDER_BASEBS) {
		if (numPasses!=2) {
			LogErr("Invalid Number of Passes (%u) in an Effect",numPasses);
			FatalAppExitA(0,"Critical error has occured. See Orbiter.log for details");
		}
	}

	// Pass 0 = Normal Mapped
	// Pass 1 = Textured
	// Pass 2 = Non-Textured

	for (DWORD pass=0;pass<numPasses;pass++) {

		if (bUseNormalMap==false && pass==0) continue; // Skip normal mapped rendering pass

		HR(FX->BeginPass(pass));

		for (DWORD g=0; g<nGrp; g++) {

			if (Grp[g]->UsrFlag & 0x2) continue;
			
			// Mesh Debugger -------------------------------------------
			//
			if (DebugControls::IsActive()) {

				if (bActiveVisual) {

					if (displ==3 && g!=selgrp) continue;

					FX->SetBool(eDebugHL, false);

					if (flags&DBG_FLAGS_HLMESH) {
						if (uCurrentMesh==selmsh) {
							FX->SetVector(eColor, &D3DXVECTOR4(0,0,0.5,1));
							FX->SetBool(eDebugHL, true);
						}
					}
					if (flags&DBG_FLAGS_HLGROUP) {
						if (g==selgrp && uCurrentMesh==selmsh) {
							FX->SetVector(eColor, &D3DXVECTOR4(0,0.5,0,1));
							FX->SetBool(eDebugHL, true);
						}
					}
				}
			}


			// Render group ----------------------------------------------
			//

			DWORD ti=Grp[g]->TexIdx;
			DWORD tni=Grp[g]->TexIdxEx[0];

			if (ti==0 && tni!=0) {
				LogErr("Night texture exists without day texture");
				FatalAppExitA(0,"Critical error has occured. See Orbiter.log for details");
			}

			if (tni && Grp[g]->TexMixEx[0]<0.5f) tni=0;
		
			if (Tex[ti]==NULL || ti==0) bTextured = false; 
			else						bTextured = true;
		
			if (bTextured) {
				pNorm = Tex[ti]->GetNormalMap();
				if (pNorm==NULL && pass==0) continue;
				if (pNorm!=NULL && pass==1) continue;
			}
			else {
				if (pass==0) continue;
				pNorm=NULL; 
				old_tex=NULL;
			}

			// Cull unvisible geometry ------------------------------------------------------
			//	
			if (bGroupCull) if (!D9IsBSVisible(&Grp[g]->BBox, &mWorldView, &Field)) continue;
		

			// Setup Mesh group material ==============================================================================
			//		
			if (Grp[g]->MtrlIdx==SPEC_DEFAULT) mat = &defmat;
			else							   mat = &Mtrl[Grp[g]->MtrlIdx];
		
			if (mat!=old_mat) { old_mat=mat; FX->SetValue(eMat, mat, sizeof(D3DMATERIAL9)); }
			
			// Setup Textures and Normal Maps ==========================================================================
			// 
			if (bTextured) {
				if (Tex[ti]!=old_tex) { 
					old_tex = Tex[ti]; 
					FX->SetTexture(eTex0, Tex[ti]->GetTexture());	
					if (tni && Tex[tni]) {
						FX->SetTexture(eTex1, Tex[tni]->GetTexture());
						FX->SetBool(eNight, true);
					} else FX->SetBool(eNight, false);

					if (bUseNormalMap) {
					
						pSpec = Tex[ti]->GetSpecularMap();
						pEmis = Tex[ti]->GetEmissionMap();

						if (pNorm) {
							FX->SetTexture(eTex3, pNorm);
							FX->SetBool(eNormalType, (Tex[ti]->NormalMapType()==1));
						}
						if (pSpec) FX->SetTexture(eTex4, pSpec);
						if (pEmis) FX->SetTexture(eTex5, pEmis);
						
						FX->SetBool(eUseSpec, (pSpec!=NULL));
						FX->SetBool(eUseEmis, (pEmis!=NULL));
					}
				}
			}

			// Apply Animations =========================================================================================
			// 
			// TODO: Clean up matrices
			if (Grp[g]->bTransform) {
				if (bGlobalTF)  {
					FX->SetMatrix(eGT, D3DXMatrixMultiply(&q, &mTransform, &Grp[g]->Transform));
					if (pass==0) FX->SetMatrix(eGTI, D3DXMatrixMultiply(&q, &Grp[g]->TransformInv, &mTransformInv));
				}
				else {
					FX->SetMatrix(eGT, &Grp[g]->Transform);
					if (pass==0) FX->SetMatrix(eGTI, &Grp[g]->TransformInv);
				}
			}
			else {
				FX->SetMatrix(eGT, &mTransform);
				if (pass==0) FX->SetMatrix(eGTI, &mTransformInv);
			}

		
			// Setup Mesh drawing options =================================================================================
			// 
			FX->SetBool(eTextured, bTextured);
			FX->SetBool(eFullyLit, (Grp[g]->UsrFlag&0x4)!=0);

			FX->CommitChanges();

			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, Grp[g]->VertOff, 0, Grp[g]->nVert, Grp[g]->FaceOff*3, Grp[g]->nFace);	

			gc->GetStats()->Vertices += Grp[g]->nVert;
			gc->GetStats()->Draw++;
			gc->GetStats()->MeshGrps++;
		}

		HR(FX->EndPass());
	}

	HR(FX->End());	

	if (flags&(DBG_FLAGS_BOXES|DBG_FLAGS_SPHERES)) RenderBoundingBox(dev, pW);

	FX->SetBool(eDebugHL, false);

	if (flags&DBG_FLAGS_DUALSIDED) dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
}




// ================================================================================================
// This is a rendering routine for a Exterior Mesh, non-spherical moons/asteroids
//
void D3D9Mesh::RenderVC(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW)
{
	DWORD flags=0, selmsh=0, selgrp=0, displ=0; // Debug Variables
	bool bActiveVisual=false;

	_TRACE;

	if (!pVB) return;
	Scene *scn = gc->GetScene(); 

	if (DebugControls::IsActive()) {
		flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
		selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
		displ  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);
		bActiveVisual = (pCurrentVisual==DebugControls::GetVisual());
		if (displ>0 && !bActiveVisual) return;
		if ((displ==2 || displ==3) && uCurrentMesh!=selmsh) return;
	}

	float mix = 0.0f;

	bool bTextured = false;

	D3DXMATRIX mWorldView, mWorldInverse, q, qq;
	D3DXMatrixMultiply(&mWorldView, pW, scn->GetViewMatrix());

	D3DXVECTOR4 Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	if (!D9IsAABBVisible(&BBox, &mWorldView, &Field)) return;
	
	gc->GetStats()->Meshes++;

	D3DMATERIAL9 *mat, *old_mat = NULL;
	LPD3D9CLIENTSURFACE old_tex = NULL;
	LPD3D9CLIENTSURFACE Diffuse = NULL;
	
	dev->SetVertexDeclaration(pMeshVertexDecl);
	dev->SetStreamSource(0, pVB, 0, sizeof(NMVERTEX));
	dev->SetIndices(pIB);

	if (flags&DBG_FLAGS_DUALSIDED) dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	FX->SetMatrix(eW, pW);
	FX->SetBool(eModAlpha, bModulateMatAlpha);
	FX->SetValue(eSun, sunLight, sizeof(D3D9Light));
	FX->SetInt(eLightCount, 0);
	FX->SetBool(eDebugHL, false);
	

	// ----------------------------------------------------------------
	FX->SetTechnique(eVCTech);
	// ----------------------------------------------------------------
	
	UINT numPasses = 0;
	FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE);
	FX->BeginPass(0);

	for (DWORD g=0; g<nGrp; g++) {


		// Mesh Debugger -------------------------------------------
		//
		if (DebugControls::IsActive()) {

			if (bActiveVisual) {

				if (displ==3 && g!=selgrp) continue;

				FX->SetBool(eDebugHL, false);

				if (flags&DBG_FLAGS_HLMESH) {
					if (uCurrentMesh==selmsh) {
						FX->SetVector(eColor, &D3DXVECTOR4(0,0,0.5,1));
						FX->SetBool(eDebugHL, true);
					}
				}
				if (flags&DBG_FLAGS_HLGROUP) {
					if (g==selgrp && uCurrentMesh==selmsh) {
						FX->SetVector(eColor, &D3DXVECTOR4(0,0.5,0,1));
						FX->SetBool(eDebugHL, true);
					}
				}
			}
		}



		if (Grp[g]->UsrFlag & 0x2) continue;
		
		DWORD ti=Grp[g]->TexIdx;
	
		if (Tex[ti]==NULL || ti==0) bTextured = false; 
		else						bTextured = true;
		
		
		// Cull unvisible geometry ------------------------------------------------------
		//	
		if (!D9IsBSVisible(&Grp[g]->BBox, &mWorldView, &Field)) continue;

		// Setup Mesh group material ==============================================================================
		//
		if (Grp[g]->MtrlIdx==SPEC_DEFAULT) mat = &defmat;
		else							   mat = &Mtrl[Grp[g]->MtrlIdx];
		if (mat!=old_mat) { old_mat=mat; FX->SetValue(eMat, mat, sizeof(D3DMATERIAL9)); }
		

		// Setup Textures and Normal Maps ==========================================================================
		// 
		if (bTextured) {
			if (Tex[ti]!=old_tex) { 
				old_tex = Tex[ti]; 
				FX->SetTexture(eTex0, Tex[ti]->GetTexture());
			}
		}

		// Apply Animations =========================================================================================
		// 
		if (Grp[g]->bTransform) {
			if (bGlobalTF)  FX->SetMatrix(eGT, D3DXMatrixMultiply(&q, &mTransform, &Grp[g]->Transform));
			else FX->SetMatrix(eGT, &Grp[g]->Transform);
		}
		else FX->SetMatrix(eGT, &mTransform);
		

		// Setup Mesh drawing options =================================================================================
		// 
		FX->SetBool(eTextured, bTextured);
		FX->SetBool(eFullyLit, (Grp[g]->UsrFlag&0x4)!=0);

		FX->CommitChanges();

		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, Grp[g]->VertOff, 0, Grp[g]->nVert, Grp[g]->FaceOff*3, Grp[g]->nFace);	

		gc->GetStats()->Vertices += Grp[g]->nVert;
		gc->GetStats()->Draw++;
		gc->GetStats()->MeshGrps++;
	}

	FX->EndPass();
	FX->End();	

	if (flags&(DBG_FLAGS_BOXES|DBG_FLAGS_SPHERES)) RenderBoundingBox(dev, pW);
	if (flags&DBG_FLAGS_DUALSIDED) dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);

	FX->SetBool(eDebugHL, false);
}




// ================================================================================================
// 
void D3D9Mesh::RenderBase(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW)
{
	if (!pVB) return;
	Render(dev, pW, RENDER_BASE);
}


// ================================================================================================
// 
void D3D9Mesh::RenderAsteroid(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW)
{
	if (!pVB) return;
	Render(dev, pW, RENDER_ASTEROID);
}

// ===========================================================================================
// 
void D3D9Mesh::RenderBaseTile(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW)
{
	if (!pVB) return;
	Render(dev, pW, RENDER_BASETILES);
}


// ================================================================================================
// 
void D3D9Mesh::RenderShadows(LPDIRECT3DDEVICE9 dev, float alpha, const LPD3DXMATRIX pW)
{
	if (!pVB) return;
	D3DXMATRIX q;

	gc->GetStats()->Meshes++;

	dev->SetVertexDeclaration(pMeshVertexDecl);
	dev->SetStreamSource(0, pVB, 0, sizeof(NMVERTEX));
	dev->SetIndices(pIB);

	D3DXVECTOR4 data(0,0,0,0);

	FX->SetTechnique(eShadowTech);
	FX->SetMatrix(eW, pW);
	FX->SetFloat(eMix, alpha);
	
	UINT numPasses = 0;
	FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE);
	FX->BeginPass(0);

	for (DWORD g=0; g<nGrp; g++) {

		if (Grp[g]->UsrFlag & 3) continue;  // No shadow & skip flags
		if (Grp[g]->IntFlag & 3) continue;  // No shadow & skip flags
		
		if (Grp[g]->bTransform) {
			if (bGlobalTF)  FX->SetMatrix(eGT, D3DXMatrixMultiply(&q, &mTransform, &Grp[g]->Transform));
			else			FX->SetMatrix(eGT, &Grp[g]->Transform);
		}
		else FX->SetMatrix(eGT, &mTransform);

		FX->CommitChanges();

		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, Grp[g]->VertOff, 0, Grp[g]->nVert, Grp[g]->FaceOff*3, Grp[g]->nFace); 

		gc->GetStats()->Vertices += Grp[g]->nVert;
		gc->GetStats()->Draw++;
	}

	FX->EndPass();
	FX->End();	
}



// ================================================================================================
// 

void D3D9Mesh::RenderShadowsEx(LPDIRECT3DDEVICE9 dev, float alpha, const LPD3DXMATRIX pP, const LPD3DXMATRIX pW, const D3DXVECTOR4 *light, const D3DXVECTOR4 *param)
{
	if (!pVB) return;
	D3DXMATRIX q;

	gc->GetStats()->Meshes++;

	dev->SetVertexDeclaration(pMeshVertexDecl);
	dev->SetStreamSource(0, pVB, 0, sizeof(NMVERTEX));
	dev->SetIndices(pIB);

	FX->SetTechnique(eShadowTech);
	FX->SetMatrix(eW, pW);
	FX->SetMatrix(eGT, pP);
	FX->SetFloat(eMix, alpha);
	FX->SetVector(eColor, light);
	FX->SetVector(eTexOff, param);
	
	UINT numPasses = 0;
	FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE);
	FX->BeginPass(1);

	for (DWORD g=0; g<nGrp; g++) {

		if (Grp[g]->UsrFlag & 3) continue; // No shadow & skip flags
		if (Grp[g]->IntFlag & 3) continue; // No shadow & skip flags
		
		FX->CommitChanges();
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, Grp[g]->VertOff, 0, Grp[g]->nVert, Grp[g]->FaceOff*3, Grp[g]->nFace); 
		gc->GetStats()->Vertices += Grp[g]->nVert;
		gc->GetStats()->Draw++;
	}

	FX->EndPass();
	FX->End();	
}




// ================================================================================================
// This is a rendering routine for a Exterior Mesh, non-spherical moons/asteroids
//
void D3D9Mesh::RenderBoundingBox(LPDIRECT3DDEVICE9 dev, const LPD3DXMATRIX pW)
{
	_TRACE;

	if (!pVB) return;
	if (DebugControls::IsActive()==false) return;
	
	Scene *scn = gc->GetScene(); 
	
	D3DXMATRIX q, qq;

	static D3DVECTOR poly[10] = {
		{0, 0, 0},
		{1, 0, 0},
		{1, 1, 0},
		{0, 1, 0},
		{0, 0, 0},
		{0, 0, 1},
		{1, 0, 1},
		{1, 1, 1},
		{0, 1, 1},
		{0, 0, 1}
	};

	static D3DVECTOR list[6] = {
		{1, 0, 0},
		{1, 0, 1},
		{1, 1, 0},
		{1, 1, 1},
		{0, 1, 0},
		{0, 1, 1}
	};
	
	

	DWORD flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
	DWORD selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
	DWORD selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
	bool  bSel   =  (uCurrentMesh==selmsh);


	if (flags&(DBG_FLAGS_SELVISONLY|DBG_FLAGS_SELMSHONLY|DBG_FLAGS_SELGRPONLY) && DebugControls::GetVisual()!=pCurrentVisual) return;
	if (flags&DBG_FLAGS_SELMSHONLY && !bSel) return;
	if (flags&DBG_FLAGS_SELGRPONLY && !bSel) return;

	if (flags&DBG_FLAGS_BOXES) {

		dev->SetVertexDeclaration(pPositionDecl);

		// ----------------------------------------------------------------
		FX->SetMatrix(eW, pW);
		FX->SetVector(eColor, &D3DXVECTOR4(0, 1, 0, 0.5f));	
		FX->SetTechnique(eBBTech);
		// ----------------------------------------------------------------
		
		UINT numPasses = 0;
		FX->Begin(&numPasses, D3DXFX_DONOTSAVESTATE);
		FX->BeginPass(0);

		for (DWORD g=0; g<nGrp; g++) {

			if (flags&DBG_FLAGS_SELGRPONLY && g!=selgrp) continue;
			if (Grp[g]->UsrFlag & 0x2) continue;
			
			FX->SetVector(eAttennuate, &Grp[g]->BBox.min);
			FX->SetVector(eInScatter, &Grp[g]->BBox.max);

			// Apply Animations =========================================================================================
			// 
			if (Grp[g]->bTransform) {
				if (bGlobalTF)  FX->SetMatrix(eGT, D3DXMatrixMultiply(&q, &mTransform, &Grp[g]->Transform));
				else FX->SetMatrix(eGT, &Grp[g]->Transform);
			}
			else FX->SetMatrix(eGT, &mTransform);
			

			// Setup Mesh drawing options =================================================================================
			// 
			FX->CommitChanges();

			dev->DrawPrimitiveUP(D3DPT_LINESTRIP, 9, &poly, sizeof(D3DVECTOR));	
			dev->DrawPrimitiveUP(D3DPT_LINELIST, 3, &list, sizeof(D3DVECTOR));	

			gc->GetStats()->Draw+=2;
		}

		FX->EndPass();
		FX->End();	
	}

	if (flags&DBG_FLAGS_SPHERES) {
		for (DWORD g=0; g<nGrp; g++) {
			if (flags&DBG_FLAGS_SELGRPONLY && g!=selgrp) continue;
			if (Grp[g]->UsrFlag & 0x2) continue;
			if (Grp[g]->bTransform) {
				if (bGlobalTF)  {
					D3DXMatrixMultiply(&q, &mTransform, &Grp[g]->Transform);
					D3D9Effect::RenderBoundingSphere(pW, &q, &Grp[g]->BBox.bs, &D3DXVECTOR4(0,1,0,0.75f));
				}
				else D3D9Effect::RenderBoundingSphere(pW, &Grp[g]->Transform, &Grp[g]->BBox.bs, &D3DXVECTOR4(0,1,0,0.75f));
			}
			else D3D9Effect::RenderBoundingSphere(pW, &mTransform, &Grp[g]->BBox.bs, &D3DXVECTOR4(0,1,0,0.75f));
		}
	}

	if (flags&DBG_FLAGS_BOXES) D3D9Effect::RenderBoundingBox(pW, &mTransform, &BBox.min, &BBox.max, &D3DXVECTOR4(0,0,1,0.75f));
	if (flags&DBG_FLAGS_SPHERES) D3D9Effect::RenderBoundingSphere(pW, &mTransform, &BBox.bs, &D3DXVECTOR4(0,0,1,0.75f));
}


// ===========================================================================================
//
void D3D9Mesh::BoundingBox(const NMVERTEX *vtx, DWORD n, D9BBox *box)
{
	if (!pVB) return;
	box->min.x = box->min.y = box->min.z =  1e12f;
	box->max.x = box->max.y = box->max.z = -1e12f;
	for (DWORD i=0;i<n;i++) {
		if (vtx[i].x < box->min.x) box->min.x=vtx[i].x;
		if (vtx[i].y < box->min.y) box->min.y=vtx[i].y;
		if (vtx[i].z < box->min.z) box->min.z=vtx[i].z;
		if (vtx[i].x > box->max.x) box->max.x=vtx[i].x;
		if (vtx[i].y > box->max.y) box->max.y=vtx[i].y;
		if (vtx[i].z > box->max.z) box->max.z=vtx[i].z;
	}

	box->min.w = box->max.w = 0.0f;
}


// ===========================================================================================
//
void D3D9Mesh::TransformGroup(DWORD n, const D3DXMATRIX *m)
{
	if (!pVB) return;
	
	bBSRecompute = true;

	Grp[n]->Transform = Grp[n]->Transform * (*m);
	Grp[n]->bTransform = true;
	Grp[n]->bUpdate = true;

	if (Config->UseNormalMap==1) D3DXMatrixInverse(&Grp[n]->TransformInv, NULL, &Grp[n]->Transform);
}

// ===========================================================================================
//
void D3D9Mesh::Transform(const D3DXMATRIX *m)
{
	if (!pVB) return;
	
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bGlobalTF = true;
	mTransform = mTransform * (*m);
	if (Config->UseNormalMap==1) D3DXMatrixInverse(&mTransformInv, NULL, &mTransform);
}

// ===========================================================================================
//
void D3D9Mesh::UpdateBoundingBox()
{
	if (!pVB) return;
	if (bBSRecompute==false) return;

	for (DWORD i=0;i<nGrp;i++) {
		if (Grp[i]->bUpdate || bBSRecomputeAll) {
			Grp[i]->bUpdate = false;
			if (bGlobalTF) {
				if (Grp[i]->bTransform) D9UpdateAABB(&Grp[i]->BBox, &mTransform, &Grp[i]->Transform);
				else					D9UpdateAABB(&Grp[i]->BBox, &mTransform);
			}
			else {
				if (Grp[i]->bTransform) D9UpdateAABB(&Grp[i]->BBox, &Grp[i]->Transform);
				else					D9UpdateAABB(&Grp[i]->BBox);
			}
		}
	}
	
	bBSRecomputeAll = false;
	bBSRecompute = false;

	if (nGrp==0) {
		BBox.min = D3DXVECTOR4(0,0,0,0);
		BBox.max = D3DXVECTOR4(0,0,0,0);
	}
	else {
		for (DWORD i=0;i<nGrp;i++) {
			if (Grp[i]->bTransform) {
				if (bGlobalTF) {
					D3DXMATRIX q;
					D3DXMatrixMultiply(&q, D3DXMatrixMultiply(&q, &mTransform, &Grp[i]->Transform), &mTransformInv);
					D9AddAABB(&Grp[i]->BBox, &q, &BBox, i==0);
				}
				else D9AddAABB(&Grp[i]->BBox, &Grp[i]->Transform, &BBox, i==0);
			}
			else {
				D9AddAABB(&Grp[i]->BBox, NULL, &BBox, i==0);
			}
		}
	}

	D9UpdateAABB(&BBox, &mTransform);
}


// ===========================================================================================
//
D9BBox * D3D9Mesh::GetAABB() 
{ 
	if (!pVB) return false;
	UpdateBoundingBox();
	return &BBox; 
}

// ===========================================================================================
//
D9BBox * D3D9Mesh::GetAABBTransformed() 
{ 
	if (!pVB) return false;
	UpdateBoundingBox();

	if (bGlobalTF) {
		BBoxT = BBox;
		D3DXVec3TransformCoord((LPD3DXVECTOR3)&BBoxT.min, (LPD3DXVECTOR3)&BBox.min, &mTransform);
		D3DXVec3TransformCoord((LPD3DXVECTOR3)&BBoxT.max, (LPD3DXVECTOR3)&BBox.max, &mTransform);
		return &BBoxT;
	}
	return &BBox; 
}

// ===========================================================================================
//
D3DXVECTOR3 D3D9Mesh::GetBoundingSpherePos() 
{ 
	if (!pVB) return D3DXVECTOR3(0,0,0);
	UpdateBoundingBox();
	return D3DXVECTOR3f4(BBox.bs);
}

// ===========================================================================================
//
float D3D9Mesh::GetBoundingSphereRadius() 
{ 
	if (!pVB) return 0.0f;
	UpdateBoundingBox();
	return BBox.bs.w; 
}

void D3D9Mesh::DumpTextures()
{
	LogBlu("Mesh 0x%X has %u textures",this, nTex-1);
	if (Tex[0]!=NULL) LogErr("Texture in index 0");
	for (DWORD i=1;i<nTex;i++) {
		if (Tex[i]) LogBlu("Texture %u: 0x%X (%s)", i, Tex[i], Tex[i]->GetName());
		else        LogBlu("Texture %u: NULL");
	}
}

void D3D9Mesh::DumpGroups()
{
	LogAlw("Mesh 0x%X has %u groups", this, nGrp);
	for (DWORD i=0;i<nGrp;i++) {
		LogAlw("Group(%u):",i);
		LogAlw("VertexCount = %u",Grp[i]->nVert);
		LogAlw("FaceCount = %u",Grp[i]->nFace);
	}
}