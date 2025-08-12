// Software Water Ripples
// BLUENIGHTHAWK - 2025
// Ported from Xash3D FWGS
// Credits to:	Xash3D FWGS Team
//				Overfloater

#include "hud.h"
#include "cl_util.h"
#include <algorithm>

#include "studio.h"
#include "r_studioint.h"

#include "r_ripples.h"

#include "StudioModelRenderer.h"
#include "GameStudioModelRenderer.h"

// speed up sin calculations
static float r_turbsin[] =
	{
#include "warpsin.h"
};

extern engine_studio_api_s IEngineStudio;
extern CGameStudioModelRenderer g_StudioRenderer;

#define SURF_PLANEBACK 2
#define SURF_DRAWSKY 4
#define SURF_DRAWSPRITE 8
#define SURF_DRAWTURB 0x10
#define SURF_DRAWTILED 0x20
#define SURF_DRAWBACKGROUND 0x40
#define SURF_UNDERWATER 0x80
#define SURF_DONTWARP 0x100
#define SURF_MARKED 0x800
#define BACKFACE_EPSILON 0.01

/*
====================
DetermineSurfaceStructSize

====================
*/
int CRipples::DetermineSurfaceStructSize(void)
{
	model_t* pworld = IEngineStudio.GetModelByIndex(1);
	assert(pworld);

	mplane_t* pplanes = pworld->planes;
	msurface_t* psurfaces = pworld->surfaces;

	// Try to find second texinfo ptr
	byte* psecondsurfbytedata = reinterpret_cast<byte*>(&psurfaces[1]);

	// Size of msurface_t with that stupid displaylist junk
	static const int MAXOFS = 108;

	int byteoffs = 0;
	while (byteoffs <= MAXOFS)
	{
		mplane_t** pplaneptr = reinterpret_cast<mplane_t**>(psecondsurfbytedata + byteoffs);

		int i = 0;
		for (; i < pworld->numplanes; i++)
		{
			if (&pplanes[i] == *pplaneptr)
				break;
		}

		if (i != pworld->numplanes)
		{
			break;
		}

		byteoffs++;
	}

	if (byteoffs >= MAXOFS)
	{
		gEngfuncs.Con_Printf("%s - Failed to determine msurface_t struct size.\n");
		return sizeof(msurface_t);
	}
	else
	{
		mplane_t** pfirstsurftexinfoptr = &psurfaces[0].plane;
		byte* psecondptr = reinterpret_cast<byte*>(psecondsurfbytedata) + byteoffs;
		byte* ptr = reinterpret_cast<byte*>(pfirstsurftexinfoptr);
		return ((unsigned int)psecondptr - (unsigned int)ptr);
	}
}

mleaf_t* Mod_PointInLeaf(Vector p, model_t* model) // quake's func
{
	mnode_t* node = model->nodes;
	while (1)
	{
		if (node->contents < 0)
			return (mleaf_t*)node;
		mplane_t* plane = node->plane;
		float d = DotProduct(p, plane->normal) - plane->dist;
		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}

	return NULL; // never reached
}


/*
============================================================

	HALF-LIFE SOFTWARE WATER

============================================================
*/

/*
====================
InitRipples

====================
*/
void CRipples::InitRipples()
{
	r_ripple = CVAR_CREATE("r_ripple", "1", FCVAR_CLIENTDLL | FCVAR_ARCHIVE);
	r_ripple_updatetime = CVAR_CREATE("r_ripple_updatetime", "0.05", FCVAR_CLIENTDLL | FCVAR_ARCHIVE);
	r_ripple_spawntime = CVAR_CREATE("r_ripple_spawntime", "0.1", FCVAR_CLIENTDLL | FCVAR_ARCHIVE);
	gl_texturemode = gEngfuncs.pfnGetCvarPointer("gl_texturemode");

	m_surfacestructsize = 0;
}

/*
====================
ResetRipples

====================
*/
void CRipples::ResetRipples(void)
{
	m_texturemode = (!gl_texturemode || strstr(gl_texturemode->string, "GL_NEAREST") == nullptr) ? GL_LINEAR : GL_NEAREST; 

	curbuf = buf[0];
	oldbuf = buf[1];
	m_time = m_oldtime = gEngfuncs.GetClientTime() - 0.1;
	memset(buf, 0, sizeof(buf));

	m_renderqueue.clear();
	for (auto f : m_PixBuffers)
	{
		delete[] f.second;
	}
	m_PixBuffers.clear();
	m_surfacestructsize = 0;
}

/*
====================
SwapBufs

====================
*/
void CRipples::SwapBufs(void)
{
	short* tempbufp = curbuf;
	curbuf = oldbuf;
	oldbuf = tempbufp;
}

/*
====================
SpawnNewRipple

====================
*/
void CRipples::SpawnNewRipple(int x, int y, short val)
{
#define PIXEL(x, y) (((x) & RIPPLES_CACHEWIDTH_MASK) + (((y) & RIPPLES_CACHEWIDTH_MASK) << 7))
	oldbuf[PIXEL(x, y)] += val;

	val >>= 2;
	oldbuf[PIXEL(x + 1, y)] += val;
	oldbuf[PIXEL(x - 1, y)] += val;
	oldbuf[PIXEL(x, y + 1)] += val;
	oldbuf[PIXEL(x, y - 1)] += val;
#undef PIXEL
}

/*
====================
RunRipplesAnimation

====================
*/
void CRipples::RunRipplesAnimation(const short* oldbuf, short* pbuf)
{
	size_t i = 0;
	const int w = RIPPLES_CACHEWIDTH;
	const int m = RIPPLES_TEXSIZE_MASK;

	for (i = w; i < m + w; i++, pbuf++)
	{
		*pbuf = (((int)oldbuf[(i - (w * 2)) & m] + (int)oldbuf[(i - (w + 1)) & m] + (int)oldbuf[(i - (w - 1)) & m] + (int)oldbuf[(i)&m]) >> 1) - (int)*pbuf;

		*pbuf -= (*pbuf >> 6);
	}
}

/*
====================
AnimateRipples

====================
*/
void CRipples::AnimateRipples(void)
{
	double frametime = gEngfuncs.GetClientTime() - m_time;

	m_update = r_ripple->value > 0 && frametime >= r_ripple_updatetime->value;

	if (!m_update)
		return;

	m_time = gEngfuncs.GetClientTime();

	SwapBufs();

	if (m_time - m_oldtime > r_ripple_spawntime->value)
	{
		int x, y, val;

		m_oldtime = m_time;

		x = rand() & 0x7fff;
		y = rand() & 0x7fff;
		val = rand() & 0x3ff;

		SpawnNewRipple(x, y, val);
	}

	RunRipplesAnimation(oldbuf, curbuf);
}

/*
====================
GetRippleTextureSize

====================
*/
void CRipples::GetRippleTextureSize(const texture_t* image, int* width, int* height)
{
	// try to preserve aspect ratio
	if (image->width > image->height)
	{
		*width = RIPPLES_CACHEWIDTH;
		*height = (float)image->height / image->width * RIPPLES_CACHEWIDTH;
	}
	else if (image->width < image->height)
	{
		*width = (float)image->width / image->height * RIPPLES_CACHEWIDTH;
		*height = RIPPLES_CACHEWIDTH;
	}
	else
	{
		*width = *height = RIPPLES_CACHEWIDTH;
	}
}

/*
====================
GetPixelBuffer

====================
*/
uint32_t* CRipples::GetPixelBuffer(texture_t* image)
{
	auto i = m_PixBuffers.find(image->gl_texturenum);
	if (i != m_PixBuffers.end())
	{
		return i->second;
	}

	int bufsize = image->width * image->height * 4;
	uint32_t* buf = new uint32_t[bufsize];
	memset(buf, 0, bufsize);
	glBindTexture(GL_TEXTURE_2D, image->gl_texturenum);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	m_PixBuffers.insert(std::make_pair(image->gl_texturenum, buf));
	return buf;
}

/*
====================
SetRippleTexMode

====================
*/
void CRipples::SetRippleTexMode()
{
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_texturemode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_texturemode);
}

/*
====================
UploadRipples

====================
*/
bool CRipples::UploadRipples(texture_t* image)
{
	const uint32_t* pixels;
	int y;
	int width, height, size;
	bool update = m_update;

	if ((int)r_ripple->value < 1)
	{
		glBindTexture(GL_TEXTURE_2D, image->gl_texturenum);
		SetRippleTexMode();
		return false;
	}

	pixels = GetPixelBuffer(image);

	// discard unuseful textures
	if (!pixels)
	{
		glBindTexture(GL_TEXTURE_2D, image->gl_texturenum);
		SetRippleTexMode();
		return false;
	}

	if (image->fb_texturenum == 0)
	{
		GLuint texnum = 0;
		glGenTextures(1, &texnum);

		GetRippleTextureSize(image, &width, &height);

		int bufsize = width * height * 4;
		uint32_t* buf = new uint32_t[bufsize];
		memset(buf, 0, bufsize);

		glBindTexture(GL_TEXTURE_2D, texnum);

		SetRippleTexMode();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, buf);

		delete[] buf;

		image->fb_texturenum = texnum;
		image->dt_texturenum = (GetFrameCount() - 1) & 0xFFFF;
		update = true;
	}

	glBindTexture(GL_TEXTURE_2D, image->fb_texturenum);
	SetRippleTexMode();

	// no updates this frame
	if (!update || image->dt_texturenum == (GetFrameCount() & 0xFFFF))
		return true;

	// prevent rendering texture multiple times in frame
	image->dt_texturenum = GetFrameCount() & 0xFFFF;

	GetRippleTextureSize(image, &width, &height);

	size = r_ripple->value == 1.0f ? 64 : RIPPLES_CACHEWIDTH;

	for (y = 0; y < height; y++)
	{
		int ry = (float)y / height * size;
		int x;

		for (x = 0; x < width; x++)
		{
			int rx = (float)x / width * size;
			int val = curbuf[ry * RIPPLES_CACHEWIDTH + rx] / 16;

			// transform it to texture space and get nice tiling effect
			int rpy = (y - val) % height;
			int rpx = (x + val) % width;

			int py = (float)rpy / height * image->height;
			int px = (float)rpx / width * image->width;

			if (py < 0)
				py = image->height + py;
			if (px < 0)
				px = image->width + px;

			texture[y * width + x] = pixels[py * image->width + px];
		}
	}
	//glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
	//	GL_RGBA, GL_UNSIGNED_BYTE, texture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, texture);

	return true;
}

/*
====================
RenderWater

====================
*/
void CRipples::RenderWater()
{
	DrawWorldWater();
	DrawWaterEntites();
}

/*
====================
AddEntity

====================
*/
bool CRipples::AddEntity(cl_entity_t* ent)
{
	if (m_surfacestructsize == 0)
	{
		m_surfacestructsize = DetermineSurfaceStructSize();
		HideSurface(gEngfuncs.hudGetModelByIndex(1));
	}
	// FUNC_WATER
	if (ent && ent->curstate.skin == CONTENT_WATER)
	{
		m_renderqueue.push_back(ent);
		HideSurface(ent->model);
		return true;
	}

	return false;
}

/*
====================
HideSurface

====================
*/
void CRipples::HideSurface(model_s *mdl)
{
	if (!mdl)
		return;
	byte* pfirstsurfbyteptr = reinterpret_cast<byte*>(mdl->surfaces);
	for (int i = 0; i < mdl->nummodelsurfaces; i++)
	{
		msurface_t* surf = reinterpret_cast<msurface_t*>(pfirstsurfbyteptr + m_surfacestructsize * (mdl->firstmodelsurface + i));

		if (surf && (surf->flags & SURF_MARKED) == 0 && (surf->flags & SURF_DRAWTURB) != 0)
		{
			for (auto p = surf->polys; p; p = p->next)
			{
				if (p && p->numverts > 0)
					p->numverts = -p->numverts;
			}
			surf->flags |= SURF_MARKED;
		}
	}
}

/*
====================
RotateForEntity

====================
*/
void CRipples::RotateForEntity(cl_entity_t* e)
{
	glTranslatef(e->origin[0], e->origin[1], e->origin[2]);

	glRotatef(e->angles[1], 0, 0, 1);
	glRotatef(-e->angles[0], 0, 1, 0);
	glRotatef(e->angles[2], 1, 0, 0);
}

/*
====================
FxBlend

====================
*/
int CRipples::FxBlend(cl_entity_t* e)
{
	int blend = 0;
	float offset, dist;
	Vector tmp;

	offset = ((int)e->index) * 363.0f; // Use ent index to de-sync these fx

	switch (e->curstate.renderfx)
	{
	case kRenderFxPulseSlowWide:
		blend = e->curstate.renderamt + 0x40 * sin(gEngfuncs.GetClientTime() * 2 + offset);
		break;
	case kRenderFxPulseFastWide:
		blend = e->curstate.renderamt + 0x40 * sin(gEngfuncs.GetClientTime() * 8 + offset);
		break;
	case kRenderFxPulseSlow:
		blend = e->curstate.renderamt + 0x10 * sin(gEngfuncs.GetClientTime() * 2 + offset);
		break;
	case kRenderFxPulseFast:
		blend = e->curstate.renderamt + 0x10 * sin(gEngfuncs.GetClientTime() * 8 + offset);
		break;
	case kRenderFxFadeSlow:
		//if (RP_NORMALPASS())
		{
			if (e->curstate.renderamt > 0)
				e->curstate.renderamt -= 1;
			else
				e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxFadeFast:
		//if (RP_NORMALPASS())
		{
			if (e->curstate.renderamt > 3)
				e->curstate.renderamt -= 4;
			else
				e->curstate.renderamt = 0;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidSlow:
		//if (RP_NORMALPASS())
		{
			if (e->curstate.renderamt < 255)
				e->curstate.renderamt += 1;
			else
				e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxSolidFast:
		//if (RP_NORMALPASS())
		{
			if (e->curstate.renderamt < 252)
				e->curstate.renderamt += 4;
			else
				e->curstate.renderamt = 255;
		}
		blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeSlow:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 4 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFast:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 16 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxStrobeFaster:
		blend = 20 * sin(gEngfuncs.GetClientTime() * 36 + offset);
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerSlow:
		blend = 20 * (sin(gEngfuncs.GetClientTime() * 2) + sin(gEngfuncs.GetClientTime() * 17 + offset));
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxFlickerFast:
		blend = 20 * (sin(gEngfuncs.GetClientTime() * 16) + sin(gEngfuncs.GetClientTime() * 23 + offset));
		if (blend < 0)
			blend = 0;
		else
			blend = e->curstate.renderamt;
		break;
	case kRenderFxHologram:
	case kRenderFxDistort:
		VectorCopy(e->origin, tmp);
		VectorSubtract(tmp, refdef.vieworg, tmp);
		dist = DotProduct(tmp, refdef.forward);

		// turn off distance fade
		if (e->curstate.renderfx == kRenderFxDistort)
			dist = 1;

		if (dist <= 0)
		{
			blend = 0;
		}
		else
		{
			e->curstate.renderamt = 180;
			if (dist <= 100)
				blend = e->curstate.renderamt;
			else
				blend = (int)((1.0f - (dist - 100) * (1.0f / 400.0f)) * e->curstate.renderamt);
			blend += gEngfuncs.pfnRandomLong(-32, 31);
		}
		break;
	default:
		blend = e->curstate.renderamt;
		break;
	}

	blend = std::clamp(blend, 0, 255);

	return blend;
}

/*
====================
SetRenderMode

====================
*/
void CRipples::SetRenderMode(cl_entity_t* e)
{
	float blend = FxBlend(e) / 255.0f;
	switch (e->curstate.rendermode)
	{
	case kRenderNormal:
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		break;
	case kRenderTransColor:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4ub(e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, e->curstate.renderamt);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glDisable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
		break;
	case kRenderTransAdd:
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor4f(blend, blend, blend, 1.0f);
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
		break;
	case kRenderTransAlpha:
		glEnable(GL_ALPHA_TEST);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		glDisable(GL_BLEND);
		glAlphaFunc(GL_GREATER, 0.25f);
		break;
	default:
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4f(1.0f, 1.0f, 1.0f, blend);
		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
		break;
	}
}


/*
====================
DrawWaterEntites

====================
*/
void CRipples::DrawWaterEntites()
{
	auto world = gEngfuncs.GetEntityByIndex(0);

	for (auto ent : m_renderqueue)
	{
		glPushMatrix();
		RotateForEntity(ent);
		SetRenderMode(ent);

		Vector absmax = (ent->origin + ent->curstate.maxs);

		bool underwater = refdef.vieworg[2] < absmax.z;

		byte* pfirstsurfbyteptr = reinterpret_cast<byte*>(world->model->surfaces);
		for (int i = 0; i < ent->model->nummodelsurfaces; i++)
		{
			msurface_t* surf = reinterpret_cast<msurface_t*>(pfirstsurfbyteptr + m_surfacestructsize * (ent->model->firstmodelsurface + i));	

			if (surf)
			{
				EmitWaterPolys(surf, underwater, UploadRipples(surf->texinfo->texture), ent);
			}
		}

		glPopMatrix();
	}
	m_renderqueue.clear();

	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glDisable(GL_ALPHA_TEST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub(255, 255, 255, 255);
}

/*
====================
DrawWorldWater

====================
*/
void CRipples::DrawWorldWater()
{
	auto g_pworld = gEngfuncs.GetEntityByIndex(0)->model;

	mleaf_t* leaf = Mod_PointInLeaf(g_StudioRenderer.m_vRenderOrigin, g_pworld);
	m_visframe = leaf->visframe;

	// draw world
	glEnable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	
	RecursiveDrawWaterWorld(g_pworld->nodes, g_pworld);
	
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4ub(255, 255, 255, 255);
}


/*
====================
RecursiveDrawWaterWorld

====================
*/
void CRipples::RecursiveDrawWaterWorld(mnode_t* node, model_s* pmodel)
{
	if (node->contents == CONTENTS_SOLID)
		return;

	if (node->visframe != m_visframe)
		return;

	if (node->contents < 0)
		return; // faces already marked by engine

	// recurse down the children, Order doesn't matter
	RecursiveDrawWaterWorld(node->children[0], pmodel);
	RecursiveDrawWaterWorld(node->children[1], pmodel);

	auto world = gEngfuncs.GetEntityByIndex(0);

	// draw stuff
	int c = node->numsurfaces;
	if (node->numsurfaces > 0)
	{
		byte* pfirstsurfbyteptr = reinterpret_cast<byte*>(pmodel->surfaces);
		for (int i = 0; i < node->numsurfaces; i++)
		{
			msurface_t* surf = reinterpret_cast<msurface_t*>(pfirstsurfbyteptr + m_surfacestructsize * (node->firstsurface + i));

			if (!surf || !surf->texinfo || !surf->texinfo->texture || /**surf->texinfo->texture->name != '!'*/ (surf->flags & SURF_DRAWTURB) == 0)
				continue;

			EmitWaterPolys(surf, false, UploadRipples(surf->texinfo->texture), world);
		}
	}
}

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented glpoly_t chain
=============
*/
void CRipples::EmitWaterPolys(msurface_t* warp, bool reverse, bool ripples, cl_entity_s* ent)
{
	float *v, nv, waveHeight;
	float s, t, os, ot;
	glpoly_t* p;
	int i;
	int numverts = 0;

	if (!warp->polys)
		return;

	float viewheight = refdef.vieworg[2];

	// set the current waveheight
	if (warp->polys->verts[0][2] >= viewheight)
		waveHeight = -ent->curstate.scale;
	else
		waveHeight = ent->curstate.scale;

	Vector absmax = (ent->origin + ent->curstate.maxs);

	for (p = warp->polys; p; p = p->next)
	{
		numverts = p->numverts;
		if (numverts < 0)
			numverts = -numverts;

		if (reverse)
			v = p->verts[0] + (numverts - 1) * VERTEXSIZE;
		else
			v = p->verts[0];

		glBegin(GL_POLYGON);

		for (i = 0; i < numverts; i++)
		{
			if (ent != gEngfuncs.GetEntityByIndex(0) && v[2] < absmax.z - 1.0f)
			{
				if (reverse)
					v -= VERTEXSIZE;
				else
					v += VERTEXSIZE;
				continue;
			}

			if (waveHeight)
			{
				nv = r_turbsin[(int)(gEngfuncs.GetClientTime() * 160.0f + v[1] + v[0]) & 255] + 8.0f;
				nv = (r_turbsin[(int)(v[0] * 5.0f + gEngfuncs.GetClientTime() * 171.0f - v[1]) & 255] + 8.0f) * 0.8f + nv;
				nv = nv * waveHeight + v[2];
			}
			else
				nv = v[2];

			os = v[3];
			ot = v[4];

			if (!ripples)
			{
				s = os + r_turbsin[(int)((ot * 0.125f + gEngfuncs.GetClientTime()) * TURBSCALE) & 255];
				t = ot + r_turbsin[(int)((os * 0.125f + gEngfuncs.GetClientTime()) * TURBSCALE) & 255];
			}
			else
			{
				s = os;
				t = ot;
			}

			s *= (1.0f / SUBDIVIDE_SIZE);
			t *= (1.0f / SUBDIVIDE_SIZE);

			glTexCoord2f(s, t);
			glVertex3f(v[0], v[1], nv);

			if (reverse)
				v -= VERTEXSIZE;
			else
				v += VERTEXSIZE;
		}
		glEnd();
	}
}


/*
====================
GetFrameCount

====================
*/
unsigned long CRipples::GetFrameCount()
{
	double a, b;
	int f;
	IEngineStudio.GetTimes(&f, &a, &b);
	return f;
}