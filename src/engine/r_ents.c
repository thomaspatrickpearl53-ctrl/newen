// newen - the renderer's entity list: studio models and sprites, sorted and drawn after the world
//
// Until hl.dll spawns entities (step 4), they come from the map's entity lump: anything with a
// literal model key (env_sprite, env_glow, monster_furniture, cycler...) plus a small table of
// common classnames whose model only the game DLL knows. Both are temporary.
#include "bsp.h"
#include "model.h"

static rentity_t *ents;           // static: from the map without a game DLL, mdlview, MakeStatic
static int ents_count, ents_cap;
static rentity_t *frame_ents;     // rebuilt every frame from the server's edicts
static int frame_count, frame_cap;
static int preview_index = -1;   // the mdlview entity

static rentity_t **trans;
static int trans_cap;
static cvar_t *r_drawentities;

rentity_t *R_AddEntity(void)
{
	if (ents_count == ents_cap) {
		ents_cap = ents_cap ? ents_cap * 2 : 256;
		ents = Mem_Realloc(ents, sizeof(*ents) * ents_cap);
	}
	rentity_t *e = &ents[ents_count++];
	memset(e, 0, sizeof(*e));
	e->renderamt = 255;
	e->framerate = 1;
	e->animtime = r_refdef.time;
	return e;
}

void R_ClearEntities(void)
{
	ents_count = 0;
	preview_index = -1;
	frame_count = 0;
}

void R_ClearFrameEntities(void)
{
	frame_count = 0;
}

rentity_t *R_AddFrameEntity(void)
{
	if (frame_count == frame_cap) {
		frame_cap = frame_cap ? frame_cap * 2 : 256;
		frame_ents = Mem_Realloc(frame_ents, sizeof(*frame_ents) * frame_cap);
	}
	rentity_t *e = &frame_ents[frame_count++];
	memset(e, 0, sizeof(*e));
	e->renderamt = 255;
	return e;
}

// ---------------------------------------------------------------- from the map

// Models the game DLL would set; only so maps look populated before hl.dll runs
static const struct { const char *classname, *model, *sequence; } preview_models[] = {
	{ "monster_scientist", "models/scientist.mdl", "idle1" },
	{ "monster_sitting_scientist", "models/scientist.mdl", "sitlookleft" },
	{ "monster_barney", "models/barney.mdl", "idle1" },
	{ "monster_gman", "models/gman.mdl", "idle" },
	{ "monster_zombie", "models/zombie.mdl", "idle1" },
	{ "monster_headcrab", "models/headcrab.mdl", "idle1" },
	{ "monster_houndeye", "models/houndeye.mdl", "idle1" },
	{ "monster_bullchicken", "models/bullsquid.mdl", "idle1" },
	{ "monster_alien_slave", "models/islave.mdl", "idle1" },
	{ "monster_human_grunt", "models/hgrunt.mdl", "idle1" },
	{ "monster_alien_grunt", "models/agrunt.mdl", "idle1" },
	{ "monster_barnacle", "models/barnacle.mdl", "idle1" },
	{ "monster_cockroach", "models/roach.mdl", NULL },
	{ "monster_rat", "models/bigrat.mdl", NULL },
	{ "item_healthkit", "models/w_medkit.mdl", NULL },
	{ "item_battery", "models/w_battery.mdl", NULL },
	{ "item_suit", "models/w_suit.mdl", NULL },
	{ "item_longjump", "models/w_longjump.mdl", NULL },
	{ "weapon_crowbar", "models/w_crowbar.mdl", NULL },
	{ "weapon_9mmhandgun", "models/w_9mmhandgun.mdl", NULL },
	{ "weapon_glock", "models/w_9mmhandgun.mdl", NULL },
	{ "weapon_357", "models/w_357.mdl", NULL },
	{ "weapon_9mmAR", "models/w_9mmAR.mdl", NULL },
	{ "weapon_mp5", "models/w_9mmAR.mdl", NULL },
	{ "weapon_shotgun", "models/w_shotgun.mdl", NULL },
	{ "weapon_crossbow", "models/w_crossbow.mdl", NULL },
	{ "weapon_rpg", "models/w_rpg.mdl", NULL },
	{ "weapon_gauss", "models/w_gauss.mdl", NULL },
	{ "weapon_egon", "models/w_egon.mdl", NULL },
	{ "weapon_hornetgun", "models/w_hgun.mdl", NULL },
	{ "weapon_handgrenade", "models/w_grenade.mdl", NULL },
	{ "weapon_satchel", "models/w_satchel.mdl", NULL },
	{ "weapon_tripmine", "models/v_tripmine.mdl", NULL },
	{ "weapon_snark", "models/w_sqknest.mdl", NULL },
	{ "ammo_9mmclip", "models/w_9mmclip.mdl", NULL },
	{ "ammo_9mmAR", "models/w_9mmARclip.mdl", NULL },
	{ "ammo_357", "models/w_357ammobox.mdl", NULL },
	{ "ammo_buckshot", "models/w_shotbox.mdl", NULL },
	{ "ammo_crossbow", "models/w_crossbow_clip.mdl", NULL },
	{ "ammo_rpgclip", "models/w_rpgammo.mdl", NULL },
	{ "ammo_gaussclip", "models/w_gaussammo.mdl", NULL },
	{ "ammo_ARgrenades", "models/w_ARgrenade.mdl", NULL },
};

static void RandomBody(const model_t *m, rentity_t *e)
{
	// body -1 (scientist heads and the like): a random choice in every body part
	const studiohdr_t *h = Mod_StudioHeader(m);
	e->body = 0;
	if (!h)
		return;
	const mstudiobodyparts_t *bp = STUDIO_PTR(h, h->bodypartindex, mstudiobodyparts_t);
	for (int i = 0; i < h->numbodyparts; i++)
		if (bp[i].nummodels > 1)
			e->body += (rand() % bp[i].nummodels) * (bp[i].base > 0 ? bp[i].base : 1);
}

static void SetupFromKeys(rentity_t *e, const bspentity_t *be)
{
	sscanf(BSP_ValueForKey(be, "origin"), "%f %f %f", &e->origin[0], &e->origin[1], &e->origin[2]);
	const char *ang = BSP_ValueForKey(be, "angles");
	if (*ang)
		sscanf(ang, "%f %f %f", &e->angles[0], &e->angles[1], &e->angles[2]);
	else
		e->angles[1] = (float)atof(BSP_ValueForKey(be, "angle"));
	e->rendermode = atoi(BSP_ValueForKey(be, "rendermode"));
	const char *amt = BSP_ValueForKey(be, "renderamt");
	e->renderamt = *amt ? atoi(amt) : 255;
	sscanf(BSP_ValueForKey(be, "rendercolor"), "%d %d %d", &e->rendercolor[0], &e->rendercolor[1], &e->rendercolor[2]);
	e->renderfx = atoi(BSP_ValueForKey(be, "renderfx"));
	e->scale = (float)atof(BSP_ValueForKey(be, "scale"));
	e->skin = atoi(BSP_ValueForKey(be, "skin"));
	e->body = atoi(BSP_ValueForKey(be, "body"));
}

void R_BuildMapEntities(void)
{
	R_ClearEntities();
	if (!world.loaded || SV_GameLoaded())
		return;   // the game DLL places entities itself
	int count = 0;
	for (int i = 1; i < world.numentities; i++) {
		const bspentity_t *be = &world.entities[i];
		const char *cls = BSP_ValueForKey(be, "classname");
		const char *modelname = BSP_ValueForKey(be, "model");
		const char *seqname = NULL;
		if (!*modelname || modelname[0] == '*') {
			modelname = NULL;
			for (size_t k = 0; k < sizeof(preview_models) / sizeof(preview_models[0]); k++)
				if (!stricmp(cls, preview_models[k].classname)) {
					modelname = preview_models[k].model;
					seqname = preview_models[k].sequence;
					break;
				}
			if (!modelname)
				continue;
		}
		// hl.dll's env_sprite starts off when something can trigger it, unless "start on" is set
		int spawnflags = atoi(BSP_ValueForKey(be, "spawnflags"));
		if (!stricmp(cls, "env_sprite") && *BSP_ValueForKey(be, "targetname") && !(spawnflags & 1))
			continue;

		model_t *m = Mod_ForName(modelname, false);
		if (!m || m->type == mod_brush)
			continue;
		rentity_t *e = R_AddEntity();
		e->model = m;
		SetupFromKeys(e, be);
		if (m->type == mod_studio) {
			R_StudioInitControllers(m, e);
			const char *seq = BSP_ValueForKey(be, "sequence");
			e->sequence = *seq ? atoi(seq) : 0;
			if (!*seq && seqname) {
				int s = R_StudioLookupSequence(m, seqname);
				e->sequence = s >= 0 ? s : 0;
			}
			if (e->body < 0)
				RandomBody(m, e);
			e->frame = (float)(rand() % 256);   // not everyone breathes in step
		} else {
			const char *fr = BSP_ValueForKey(be, "framerate");
			e->framerate = *fr ? (float)atof(fr) : 10.0f;
			e->frame = (float)atof(BSP_ValueForKey(be, "frame"));
		}
		count++;
	}
	Con_DPrintf("%d model and sprite entities\n", count);
}

// ---------------------------------------------------------------- drawing

static int CompareDist(const void *a, const void *b)
{
	float da = (*(rentity_t *const *)a)->dist, db = (*(rentity_t *const *)b)->dist;
	return da < db ? 1 : da > db ? -1 : 0;   // far to near
}

static qboolean IsTranslucent(const rentity_t *e)
{
	return e->rendermode != kRenderNormal;
}

static void DrawEntity(rentity_t *e)
{
	if (e->model->type == mod_studio) {
		glEnable(GL_CULL_FACE);
		R_StudioDrawModel(e);
	} else {
		glDisable(GL_CULL_FACE);   // sprites are seen from both sides
		R_SpriteDrawModel(e);
	}
}

void R_DrawEntities(void)
{
	int total = ents_count + frame_count;
	if (!r_drawentities->value || !total)
		return;
	if (trans_cap < total) {
		trans_cap = total * 2;
		trans = Mem_Realloc(trans, sizeof(*trans) * trans_cap);
	}

	glEnable(GL_DEPTH_TEST);
	glCullFace(GL_FRONT);
	qglActiveTexture(GL_TEXTURE0);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	int ntrans = 0;
	for (int i = 0; i < total; i++) {
		rentity_t *e = i < ents_count ? &ents[i] : &frame_ents[i - ents_count];
		if (!e->model || (IsTranslucent(e) && e->renderamt <= 0))
			continue;
		qboolean culled = e->model->type == mod_studio ? R_StudioCull(e) : R_SpriteCull(e);
		if (culled)
			continue;
		float r = e->model->radius > 8 ? e->model->radius : 8;
		vec3_t mins = { e->origin[0] - r, e->origin[1] - r, e->origin[2] - r };
		vec3_t maxs = { e->origin[0] + r, e->origin[1] + r, e->origin[2] + r };
		if (!R_BoxVisible(mins, maxs))
			continue;
		if (!IsTranslucent(e)) {
			DrawEntity(e);
			continue;
		}
		vec3_t d;
		VectorSubtract(e->origin, r_refdef.origin, d);
		e->dist = DotProduct(d, d);
		trans[ntrans++] = e;
	}
	qsort(trans, ntrans, sizeof(*trans), CompareDist);
	for (int i = 0; i < ntrans; i++)
		DrawEntity(trans[i]);

	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDepthMask(GL_TRUE);
	glColor4f(1, 1, 1, 1);
}

// ---------------------------------------------------------------- commands

static void MdlView_f(void)
{
	if (Cmd_Argc() < 2) {
		if (preview_index >= 0 && preview_index < ents_count)
			ents[preview_index].model = NULL;
		preview_index = -1;
		Con_Printf("usage: mdlview <model> [sequence number or name] [body] [skin]   (no arguments removes it)\n");
		return;
	}
	if (!world.loaded) {
		Con_Printf("Load a map first\n");
		return;
	}
	const char *name = Cmd_Argv(1);
	const char *path = strchr(name, '/') ? name : va("models/%s.mdl", name);
	model_t *m = Mod_ForName(path, false);
	if (!m || m->type == mod_brush)
		return;

	rentity_t *e;
	if (preview_index >= 0 && preview_index < ents_count) {
		e = &ents[preview_index];
		memset(e, 0, sizeof(*e));
		e->renderamt = 255;
		e->framerate = 1;
		e->animtime = r_refdef.time;
	} else {
		e = R_AddEntity();
		preview_index = (int)(e - ents);
	}
	e->model = m;
	// in front of the camera, at the feet of a standing player, facing back
	// from the angles, not r_refdef.forward: that is only updated when a frame is drawn
	vec3_t f;
	AngleVectors(r_refdef.angles, f, NULL, NULL);
	f[2] = 0;
	if (VectorNormalize(f) == 0)
		VectorSet(f, 1, 0, 0);
	VectorMA(r_refdef.origin, 120, f, e->origin);
	e->origin[2] -= m->type == mod_studio ? 64 : 0;
	e->angles[1] = r_refdef.angles[1] + 180;
	if (m->type == mod_sprite) {
		e->rendermode = m->sprite.texFormat == SPR_ADDITIVE ? kRenderTransAdd : kRenderNormal;
		e->framerate = 10;
	} else {
		R_StudioInitControllers(m, e);
		if (Cmd_Argc() > 2) {
			const char *s = Cmd_Argv(2);
			e->sequence = (*s >= '0' && *s <= '9') ? atoi(s) : R_StudioLookupSequence(m, s);
			if (e->sequence < 0) {
				Con_Printf("%s has no sequence %s\n", m->name, s);
				e->sequence = 0;
			}
		}
		if (Cmd_Argc() > 3)
			e->body = atoi(Cmd_Argv(3));
		if (Cmd_Argc() > 4)
			e->skin = atoi(Cmd_Argv(4));
	}
	Mod_PrintInfo(m);
}

static void EntList_f(void)
{
	for (int i = 0; i < ents_count; i++) {
		const rentity_t *e = &ents[i];
		if (!e->model)
			continue;
		Con_Printf("%3d %-28s at %6.0f %6.0f %6.0f  seq %d  mode %d amt %d\n", i, e->model->name,
			e->origin[0], e->origin[1], e->origin[2], e->sequence, e->rendermode, e->renderamt);
	}
}

void R_InitEntities(void)
{
	r_drawentities = Cvar_Get("r_drawentities", "1", 0, "draws brush entities, models and sprites");
	Cmd_AddCommand("mdlview", MdlView_f, "mdlview <model> [sequence] [body] [skin] - shows a model or sprite in front of you");
	Cmd_AddCommand("r_entlist", EntList_f, "lists the models and sprites the renderer draws");
}
