#pragma once

#include "PlatformHeaders.h"
#include "SDL2/SDL_opengl.h"

#include <map>
#include <vector>

#include "com_model.h"

#define TURBSCALE (256.0f / (M_PI))
#define SUBDIVIDE_SIZE 64

#define RIPPLES_CACHEWIDTH_BITS 7
#define RIPPLES_CACHEWIDTH (1 << RIPPLES_CACHEWIDTH_BITS)
#define RIPPLES_CACHEWIDTH_MASK ((RIPPLES_CACHEWIDTH) - 1)
#define RIPPLES_TEXSIZE (RIPPLES_CACHEWIDTH * RIPPLES_CACHEWIDTH)
#define RIPPLES_TEXSIZE_MASK (RIPPLES_TEXSIZE - 1)

static_assert(RIPPLES_TEXSIZE == 0x4000, "fix the algorithm to work with custom resolution");

class CRipples
{
public:
	// HUD_Init
	void InitRipples();

	// HUD_VidInit and HUD_Shutdown
	void ResetRipples(void);
	// HUD_Redraw
	void AnimateRipples(void);

	// HUD_DrawTransparentTriangles
	void RenderWater();

	// HUD_AddEntity
	bool AddEntity(cl_entity_t* ent);

public:
	// Set in V_CalcRefDef
	ref_params_s refdef;

private:
	void SwapBufs(void);
	void SpawnNewRipple(int x, int y, short val);
	void RunRipplesAnimation(const short* oldbuf, short* pbuf);
	void GetRippleTextureSize(const texture_t* image, int* width, int* height);
	void SetRippleTexMode();
	uint32_t* GetPixelBuffer(texture_t* image);
	bool UploadRipples(texture_t* image);
	void DrawWaterEntites();
	void DrawWorldWater();
	void RecursiveDrawWaterWorld(mnode_t* node, model_s* pmodel);

	void HideSurface(model_s* mdl);

	int DetermineSurfaceStructSize(void);

	unsigned long GetFrameCount();

	void RotateForEntity(cl_entity_t* e);
	int FxBlend(cl_entity_t* e);
	void SetRenderMode(cl_entity_t* e);

	void EmitWaterPolys(msurface_t* warp, bool reverse, bool ripples, cl_entity_s* ent);

private:
	std::map<GLuint, uint32_t*> m_PixBuffers;

	cvar_t *r_ripple, *r_ripple_updatetime, *r_ripple_spawntime, *gl_texturemode;

	double m_time;
	double m_oldtime;

	int m_visframe;

	short *curbuf, *oldbuf;
	short buf[2][RIPPLES_TEXSIZE];
	bool m_update;

	int m_surfacestructsize;

	int m_texturemode;

	uint32_t texture[RIPPLES_TEXSIZE];

	std::vector<cl_entity_t*> m_renderqueue;
};

inline CRipples g_Ripples;