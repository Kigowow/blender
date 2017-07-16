/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008, Blender Foundation
 * This is a new part of Blender
 *
 * Contributor(s): Joshua Leung, Antonio Vazquez
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/gpencil.c
 *  \ingroup bke
 */

 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_math_vector.h"
#include "BLI_string_utils.h"
#include "BLI_rand.h"

#include "BLT_translation.h"

#include "DNA_anim_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_userdef_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_modifier_types.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_colortools.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_object.h"

 /* Draw Engine */
void(*BKE_gpencil_batch_cache_dirty_cb)(bGPdata *gpd) = NULL;
void(*BKE_gpencil_batch_cache_free_cb)(bGPdata *gpd) = NULL;

void BKE_gpencil_batch_cache_dirty(bGPdata *gpd)
{
	if ((gpd) && (gpd->batch_cache)) {
		BKE_gpencil_batch_cache_dirty_cb(gpd);
	}
}

void BKE_gpencil_batch_cache_free(bGPdata *gpd)
{
	if ((gpd) && (gpd->batch_cache)) {
		BKE_gpencil_batch_cache_free_cb(gpd);
	}
}

/* ************************************************** */
/* GENERAL STUFF */

/* --------- Memory Management ------------ */

/* free stroke, doesn't unlink from any listbase */
void BKE_gpencil_free_stroke(bGPDstroke *gps)
{
	if (gps == NULL) {
		return;
	}

	/* free stroke memory arrays, then stroke itself */
	if (gps->points)
		MEM_freeN(gps->points);
	if (gps->triangles)
		MEM_freeN(gps->triangles);

	MEM_freeN(gps);
}

/* Free strokes belonging to a gp-frame */
bool BKE_gpencil_free_strokes(bGPDframe *gpf)
{
	bGPDstroke *gps_next;
	bool changed = (BLI_listbase_is_empty(&gpf->strokes) == false);

	/* free strokes */
	for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps_next) {
		gps_next = gps->next;
		BKE_gpencil_free_stroke(gps);
	}
	BLI_listbase_clear(&gpf->strokes);

	return changed;
}

/* Free all of a gp-layer's frames */
void BKE_gpencil_free_frames(bGPDlayer *gpl)
{
	bGPDframe *gpf_next;
	
	/* error checking */
	if (gpl == NULL) return;
	
	/* free frames */
	for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf_next) {
		gpf_next = gpf->next;
		
		/* free strokes and their associated memory */
		BKE_gpencil_free_strokes(gpf);
		BLI_freelinkN(&gpl->frames, gpf);
	}
	gpl->actframe = NULL;
}

/* Free all of a gp-colors */
static void free_gpencil_colors(bGPDpalette *palette)
{
	/* error checking */
	if (palette == NULL) {
		return;
	}

	/* free colors */
	BLI_freelistN(&palette->colors);
}

/* Free all of the gp-palettes and colors */
void BKE_gpencil_free_palettes(ListBase *list)
{
	bGPDpalette *palette_next;

	/* error checking */
	if (list == NULL) {
		return;
	}

	/* delete palettes */
	for (bGPDpalette *palette = list->first; palette; palette = palette_next) {
		palette_next = palette->next;
		/* free palette colors */
		free_gpencil_colors(palette);

		MEM_freeN(palette);
	}
	BLI_listbase_clear(list);
}

/* Free all of the gp-brushes for a viewport (list should be &gpd->brushes or so) */
void BKE_gpencil_free_brushes(ListBase *list)
{
	bGPDbrush *brush_next;

	/* error checking */
	if (list == NULL) {
		return;
	}

	/* delete brushes */
	for (bGPDbrush *brush = list->first; brush; brush = brush_next) {
		brush_next = brush->next;
		/* free curves */
		if (brush->cur_sensitivity) {
			curvemapping_free(brush->cur_sensitivity);
		}
		if (brush->cur_strength) {
			curvemapping_free(brush->cur_strength);
		}
		if (brush->cur_jitter) {
			curvemapping_free(brush->cur_jitter);
		}

		MEM_freeN(brush);
	}
	BLI_listbase_clear(list);
}

/* Free all of the gp-layers for a viewport (list should be &gpd->layers or so) */
void BKE_gpencil_free_layers(ListBase *list)
{
	bGPDlayer *gpl_next;
	
	/* error checking */
	if (list == NULL) return;
	
	/* delete layers */
	for (bGPDlayer *gpl = list->first; gpl; gpl = gpl_next) {
		gpl_next = gpl->next;
		
		/* free layers and their data */
		BKE_gpencil_free_frames(gpl);
		BLI_freelinkN(list, gpl);
	}
}

/** Free (or release) any data used by this grease pencil (does not free the gpencil itself). */
void BKE_gpencil_free(bGPdata *gpd, bool free_all)
{
	/* clear animation data */
	BKE_animdata_free(&gpd->id, false);

	/* free layers */
	BKE_gpencil_free_layers(&gpd->layers);

	/* free all data */
	if (free_all) {
		/* clear cache */
		BKE_gpencil_batch_cache_free(gpd);
		BKE_gpencil_free_palettes(&gpd->palettes);
	}
}

/* -------- Container Creation ---------- */

/* add a new gp-frame to the given layer */
bGPDframe *BKE_gpencil_frame_addnew(bGPDlayer *gpl, int cframe)
{
	bGPDframe *gpf = NULL, *gf = NULL;
	short state = 0;
	
	/* error checking */
	if (gpl == NULL)
		return NULL;
		
	/* allocate memory for this frame */
	gpf = MEM_callocN(sizeof(bGPDframe), "bGPDframe");
	gpf->framenum = cframe;
	
	/* find appropriate place to add frame */
	if (gpl->frames.first) {
		for (gf = gpl->frames.first; gf; gf = gf->next) {
			/* check if frame matches one that is supposed to be added */
			if (gf->framenum == cframe) {
				state = -1;
				break;
			}
			
			/* if current frame has already exceeded the frame to add, add before */
			if (gf->framenum > cframe) {
				BLI_insertlinkbefore(&gpl->frames, gf, gpf);
				state = 1;
				break;
			}
		}
	}
	
	/* check whether frame was added successfully */
	if (state == -1) {
		printf("Error: Frame (%d) existed already for this layer. Using existing frame\n", cframe);
		
		/* free the newly created one, and use the old one instead */
		MEM_freeN(gpf);
		
		/* return existing frame instead... */
		BLI_assert(gf != NULL);
		gpf = gf;
	}
	else if (state == 0) {
		/* add to end then! */
		BLI_addtail(&gpl->frames, gpf);
	}
	
	/* return frame */
	return gpf;
}

/* add a copy of the active gp-frame to the given layer */
bGPDframe *BKE_gpencil_frame_addcopy(bGPDlayer *gpl, int cframe)
{
	bGPDframe *new_frame;
	bool found = false;
	
	/* Error checking/handling */
	if (gpl == NULL) {
		/* no layer */
		return NULL;
	}
	else if (gpl->actframe == NULL) {
		/* no active frame, so just create a new one from scratch */
		return BKE_gpencil_frame_addnew(gpl, cframe);
	}
	
	/* Create a copy of the frame */
	new_frame = BKE_gpencil_frame_duplicate(gpl->actframe);
	
	/* Find frame to insert it before */
	for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf->next) {
		if (gpf->framenum > cframe) {
			/* Add it here */
			BLI_insertlinkbefore(&gpl->frames, gpf, new_frame);
			
			found = true;
			break;
		}
		else if (gpf->framenum == cframe) {
			/* This only happens when we're editing with framelock on...
			 * - Delete the new frame and don't do anything else here...
			 */
			BKE_gpencil_free_strokes(new_frame);
			MEM_freeN(new_frame);
			new_frame = NULL;
			
			found = true;
			break;
		}
	}
	
	if (found == false) {
		/* Add new frame to the end */
		BLI_addtail(&gpl->frames, new_frame);
	}
	
	/* Ensure that frame is set up correctly, and return it */
	if (new_frame) {
		new_frame->framenum = cframe;
		gpl->actframe = new_frame;
	}
	
	return new_frame;
}

/* add a new gp-layer and make it the active layer */
bGPDlayer *BKE_gpencil_layer_addnew(bGPdata *gpd, const char *name, bool setactive)
{
	bGPDlayer *gpl;
	
	/* check that list is ok */
	if (gpd == NULL)
		return NULL;
		
	/* allocate memory for frame and add to end of list */
	gpl = MEM_callocN(sizeof(bGPDlayer), "bGPDlayer");
	
	/* add to datablock */
	BLI_addtail(&gpd->layers, gpl);
	
	/* set basic settings */
	copy_v4_v4(gpl->color, U.gpencil_new_layer_col);
	/* Since GPv2 thickness must be 0 */
	gpl->thickness = 0;

	gpl->opacity = 1.0f;

	/* onion-skinning settings */
	if (gpd->flag & GP_DATA_SHOW_ONIONSKINS)
		gpl->flag |= GP_LAYER_ONIONSKIN;
	
	gpl->flag |= (GP_LAYER_GHOST_PREVCOL | GP_LAYER_GHOST_NEXTCOL);
	
	ARRAY_SET_ITEMS(gpl->gcolor_prev, 0.145098f, 0.419608f, 0.137255f); /* green */
	ARRAY_SET_ITEMS(gpl->gcolor_next, 0.125490f, 0.082353f, 0.529412f); /* blue */
	
	/* auto-name */
	BLI_strncpy(gpl->info, name, sizeof(gpl->info));
	BLI_uniquename(&gpd->layers, gpl, DATA_("GP_Layer"), '.', offsetof(bGPDlayer, info), sizeof(gpl->info));
	
	/* make this one the active one */
	if (setactive)
		BKE_gpencil_layer_setactive(gpd, gpl);
	
	/* return layer */
	return gpl;
}

/* add a new gp-palette and make it the active */
bGPDpalette *BKE_gpencil_palette_addnew(bGPdata *gpd, const char *name, bool setactive)
{
	bGPDpalette *palette;

	/* check that list is ok */
	if (gpd == NULL) {
		return NULL;
	}

	/* allocate memory and add to end of list */
	palette = MEM_callocN(sizeof(bGPDpalette), "bGPDpalette");

	/* add to datablock */
	BLI_addtail(&gpd->palettes, palette);

	/* set basic settings */
	/* auto-name */
	BLI_strncpy(palette->info, name, sizeof(palette->info));
	BLI_uniquename(&gpd->palettes, palette, DATA_("GP_Palette"), '.', offsetof(bGPDpalette, info),
	               sizeof(palette->info));

	/* make this one the active one */
	/* NOTE: Always make this active if there's nothing else yet (T50123) */
	if ((setactive) || (gpd->palettes.first == gpd->palettes.last)) {
		BKE_gpencil_palette_setactive(gpd, palette);
	}

	/* return palette */
	return palette;
}

/* create a set of default drawing brushes with predefined presets */
void BKE_gpencil_brush_init_presets(ToolSettings *ts)
{
	bGPDbrush *brush;
	float curcolor[3];
	ARRAY_SET_ITEMS(curcolor, 1.0f, 1.0f, 1.0f);

	/* Basic brush */
	brush = BKE_gpencil_brush_addnew(ts, "Basic", true);
	brush->thickness = 3.0f;
	brush->flag &= ~GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 1.0f;
	brush->flag |= GP_BRUSH_USE_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 1.0f;
	brush->flag |= ~GP_BRUSH_USE_STENGTH_PRESSURE;

	brush->draw_random_press = 0.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = 0.0f;
	brush->draw_angle_factor = 0.0f;

	brush->draw_smoothfac = 0.0f;
	brush->draw_smoothlvl = 1;
	brush->sublevel = 0;
	brush->draw_random_sub = 0.0f;
	copy_v3_v3(brush->curcolor, curcolor);

	/* Pencil brush */
	brush = BKE_gpencil_brush_addnew(ts, "Pencil", false);
	brush->thickness = 7.0f;
	brush->flag &= ~GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 1.0f;
	brush->flag |= GP_BRUSH_USE_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 0.7f;
	brush->flag |= GP_BRUSH_USE_STENGTH_PRESSURE;

	brush->draw_random_press = 0.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = 0.0f;
	brush->draw_angle_factor = 0.0f;

	brush->draw_smoothfac = 1.0f;
	brush->draw_smoothlvl = 2;
	brush->sublevel = 2;
	brush->draw_random_sub = 0.0f;
	copy_v3_v3(brush->curcolor, curcolor);

	/* Ink brush */
	brush = BKE_gpencil_brush_addnew(ts, "Ink", false);
	brush->thickness = 7.0f;
	brush->flag &= ~GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 1.6f;
	brush->flag |= GP_BRUSH_USE_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 1.0f;
	brush->flag &= ~GP_BRUSH_USE_STENGTH_PRESSURE;

	brush->draw_random_press = 0.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = 0.0f;
	brush->draw_angle_factor = 0.0f;

	brush->draw_smoothfac = 1.1f;
	brush->draw_smoothlvl = 2;
	brush->sublevel = 2;
	brush->draw_random_sub = 0.0f;
	copy_v3_v3(brush->curcolor, curcolor);

	/* Ink Noise brush */
	brush = BKE_gpencil_brush_addnew(ts, "Ink noise", false);
	brush->thickness = 6.0f;
	brush->flag |= GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 1.611f;
	brush->flag |= GP_BRUSH_USE_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 1.0f;
	brush->flag |= GP_BRUSH_USE_STENGTH_PRESSURE;

	brush->draw_random_press = 1.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = 0.0f;
	brush->draw_angle_factor = 0.0f;

	brush->draw_smoothfac = 1.1f;
	brush->draw_smoothlvl = 2;
	brush->sublevel = 2;
	brush->draw_random_sub = 0.0f;
	copy_v3_v3(brush->curcolor, curcolor);

	/* Marker brush */
	brush = BKE_gpencil_brush_addnew(ts, "Marker", false);
	brush->thickness = 10.0f;
	brush->flag &= ~GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 2.0f;
	brush->flag &= ~GP_BRUSH_USE_PRESSURE;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 1.0f;
	brush->flag &= ~GP_BRUSH_USE_STENGTH_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->draw_random_press = 0.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = M_PI_4; /* 45 degrees */
	brush->draw_angle_factor = 1.0f;

	brush->draw_smoothfac = 1.0f;
	brush->draw_smoothlvl = 2;
	brush->sublevel = 2;
	brush->draw_random_sub = 0.0f;
	copy_v3_v3(brush->curcolor, curcolor);

	/* Crayon brush */
	brush = BKE_gpencil_brush_addnew(ts, "Crayon", false);
	brush->thickness = 10.0f;
	brush->flag &= ~GP_BRUSH_USE_RANDOM_PRESSURE;
	brush->draw_sensitivity = 3.0f;
	brush->flag &= ~GP_BRUSH_USE_PRESSURE;
	brush->flag |= GP_BRUSH_ENABLE_CURSOR;

	brush->flag &= ~GP_BRUSH_USE_RANDOM_STRENGTH;
	brush->draw_strength = 0.140f;
	brush->flag |= GP_BRUSH_USE_STENGTH_PRESSURE;

	brush->draw_random_press = 0.0f;

	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	brush->draw_angle = 0.0f;
	brush->draw_angle_factor = 0.0f;

	brush->draw_smoothfac = 0.0f;
	brush->draw_smoothlvl = 1;
	brush->sublevel = 2;
	brush->draw_random_sub = 0.5f;
	copy_v3_v3(brush->curcolor, curcolor);
}

/* add a new gp-brush and make it the active */
bGPDbrush *BKE_gpencil_brush_addnew(ToolSettings *ts, const char *name, bool setactive)
{
	bGPDbrush *brush;

	/* check that list is ok */
	if (ts == NULL) {
		return NULL;
	}

	/* allocate memory and add to end of list */
	brush = MEM_callocN(sizeof(bGPDbrush), "bGPDbrush");

	/* add to datablock */
	BLI_addtail(&ts->gp_brushes, brush);

	/* set basic settings */
	brush->thickness = 3;
	brush->draw_smoothlvl = 1;
	brush->flag |= GP_BRUSH_USE_PRESSURE;
	brush->draw_sensitivity = 1.0f;
	brush->draw_strength = 1.0f;
	brush->flag |= GP_BRUSH_USE_STENGTH_PRESSURE;
	brush->draw_jitter = 0.0f;
	brush->flag |= GP_BRUSH_USE_JITTER_PRESSURE;

	/* curves */
	brush->cur_sensitivity = curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
	brush->cur_strength = curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
	brush->cur_jitter = curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);

	/* auto-name */
	BLI_strncpy(brush->info, name, sizeof(brush->info));
	BLI_uniquename(&ts->gp_brushes, brush, DATA_("GP_Brush"), '.', offsetof(bGPDbrush, info), sizeof(brush->info));

	/* make this one the active one */
	if (setactive) {
		BKE_gpencil_brush_setactive(ts, brush);
	}

	/* return brush */
	return brush;
}

/* add a new gp-palettecolor and make it the active */
bGPDpalettecolor *BKE_gpencil_palettecolor_addnew(bGPDpalette *palette, const char *name, bool setactive)
{
	bGPDpalettecolor *palcolor;

	/* check that list is ok */
	if (palette == NULL) {
		return NULL;
	}

	/* allocate memory and add to end of list */
	palcolor = MEM_callocN(sizeof(bGPDpalettecolor), "bGPDpalettecolor");

	/* add to datablock */
	BLI_addtail(&palette->colors, palcolor);

	/* set basic settings */
	copy_v4_v4(palcolor->color, U.gpencil_new_layer_col);
	ARRAY_SET_ITEMS(palcolor->fill, 1.0f, 1.0f, 1.0f);

	/* auto-name */
	BLI_strncpy(palcolor->info, name, sizeof(palcolor->info));
	BLI_uniquename(&palette->colors, palcolor, DATA_("Color"), '.', offsetof(bGPDpalettecolor, info),
	               sizeof(palcolor->info));

	/* make this one the active one */
	if (setactive) {
		BKE_gpencil_palettecolor_setactive(palette, palcolor);
	}

	/* return palette color */
	return palcolor;
}

/* add a new gp-datablock */
bGPdata *BKE_gpencil_data_addnew(const char name[])
{
	bGPdata *gpd;
	
	/* allocate memory for a new block */
	gpd = BKE_libblock_alloc(G.main, ID_GD, name);
	
	/* initial settings */
	gpd->flag = (GP_DATA_DISPINFO | GP_DATA_EXPAND);
	
	/* for now, stick to view is also enabled by default
	 * since this is more useful...
	 */
	gpd->flag |= GP_DATA_VIEWALIGN;
	gpd->xray_mode = GP_XRAY_3DSPACE;
	gpd->batch_cache = NULL;
	
	return gpd;
}

/* -------- Data Duplication ---------- */

/* make a copy of a given gpencil frame */
bGPDframe *BKE_gpencil_frame_duplicate(const bGPDframe *gpf_src)
{
	bGPDstroke *gps_dst;
	bGPDframe *gpf_dst;
	
	/* error checking */
	if (gpf_src == NULL) {
		return NULL;
	}
		
	/* make a copy of the source frame */
	gpf_dst = MEM_dupallocN(gpf_src);
	gpf_dst->prev = gpf_dst->next = NULL;
	
	/* copy strokes */
	BLI_listbase_clear(&gpf_dst->strokes);
	for (bGPDstroke *gps_src = gpf_src->strokes.first; gps_src; gps_src = gps_src->next) {
		/* make copy of source stroke, then adjust pointer to points too */
		gps_dst = MEM_dupallocN(gps_src);
		gps_dst->points = MEM_dupallocN(gps_src->points);
		gps_dst->triangles = MEM_dupallocN(gps_src->triangles);
		gps_dst->flag |= GP_STROKE_RECALC_CACHES;
		BLI_addtail(&gpf_dst->strokes, gps_dst);
	}
	
	/* return new frame */
	return gpf_dst;
}

/* make a copy of a given gpencil brush */
bGPDbrush *BKE_gpencil_brush_duplicate(const bGPDbrush *brush_src)
{
	bGPDbrush *brush_dst;

	/* error checking */
	if (brush_src == NULL) {
		return NULL;
	}

	/* make a copy of source brush */
	brush_dst = MEM_dupallocN(brush_src);
	brush_dst->prev = brush_dst->next = NULL;
	/* make a copy of curves */
	brush_dst->cur_sensitivity = curvemapping_copy(brush_src->cur_sensitivity);
	brush_dst->cur_strength = curvemapping_copy(brush_src->cur_strength);
	brush_dst->cur_jitter = curvemapping_copy(brush_src->cur_jitter);

	/* return new brush */
	return brush_dst;
}

/* make a copy of a given gpencil palette */
bGPDpalette *BKE_gpencil_palette_duplicate(const bGPDpalette *palette_src)
{
	bGPDpalette *palette_dst;
	const bGPDpalettecolor *palcolor_src;
	bGPDpalettecolor *palcolord_dst;

	/* error checking */
	if (palette_src == NULL) {
		return NULL;
	}

	/* make a copy of source palette */
	palette_dst = MEM_dupallocN(palette_src);
	palette_dst->prev = palette_dst->next = NULL;

	/* copy colors */
	BLI_listbase_clear(&palette_dst->colors);
	for (palcolor_src = palette_src->colors.first; palcolor_src; palcolor_src = palcolor_src->next) {
		/* make a copy of source */
		palcolord_dst = MEM_dupallocN(palcolor_src);
		BLI_addtail(&palette_dst->colors, palcolord_dst);
	}

	/* return new palette */
	return palette_dst;
}
/* make a copy of a given gpencil layer */
bGPDlayer *BKE_gpencil_layer_duplicate(const bGPDlayer *gpl_src)
{
	const bGPDframe *gpf_src;
	bGPDframe *gpf_dst;
	bGPDlayer *gpl_dst;
	
	/* error checking */
	if (gpl_src == NULL) {
		return NULL;
	}
		
	/* make a copy of source layer */
	gpl_dst = MEM_dupallocN(gpl_src);
	gpl_dst->prev = gpl_dst->next = NULL;
	
	/* copy frames */
	BLI_listbase_clear(&gpl_dst->frames);
	for (gpf_src = gpl_src->frames.first; gpf_src; gpf_src = gpf_src->next) {
		/* make a copy of source frame */
		gpf_dst = BKE_gpencil_frame_duplicate(gpf_src);
		BLI_addtail(&gpl_dst->frames, gpf_dst);
		
		/* if source frame was the current layer's 'active' frame, reassign that too */
		if (gpf_src == gpl_dst->actframe)
			gpl_dst->actframe = gpf_dst;
	}
	
	/* return new layer */
	return gpl_dst;
}

/* make a copy of a given gpencil datablock */
bGPdata *BKE_gpencil_data_duplicate(Main *bmain, const bGPdata *gpd_src, bool internal_copy)
{
	const bGPDlayer *gpl_src;
	bGPDlayer *gpl_dst;
	bGPdata *gpd_dst;

	/* error checking */
	if (gpd_src == NULL) {
		return NULL;
	}
	
	/* make a copy of the base-data */
	if (internal_copy) {
		/* make a straight copy for undo buffers used during stroke drawing */
		gpd_dst = MEM_dupallocN(gpd_src);
	}
	else {
		/* make a copy when others use this */
		gpd_dst = BKE_libblock_copy(bmain, &gpd_src->id);
		gpd_dst->batch_cache = NULL;
	}
	
	/* copy layers */
	BLI_listbase_clear(&gpd_dst->layers);
	for (gpl_src = gpd_src->layers.first; gpl_src; gpl_src = gpl_src->next) {
		/* make a copy of source layer and its data */
		gpl_dst = BKE_gpencil_layer_duplicate(gpl_src);
		BLI_addtail(&gpd_dst->layers, gpl_dst);
	}
	
	/* return new */
	return gpd_dst;
}

void BKE_gpencil_make_local(Main *bmain, bGPdata *gpd, const bool lib_local)
{
	BKE_id_make_local_generic(bmain, &gpd->id, true, lib_local);
}

/* -------- GP-Stroke API --------- */

/* ensure selection status of stroke is in sync with its points */
void BKE_gpencil_stroke_sync_selection(bGPDstroke *gps)
{
	bGPDspoint *pt;
	int i;
	
	/* error checking */
	if (gps == NULL)
		return;
	
	/* we'll stop when we find the first selected point,
	 * so initially, we must deselect
	 */
	gps->flag &= ~GP_STROKE_SELECT;
	
	for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
		if (pt->flag & GP_SPOINT_SELECT) {
			gps->flag |= GP_STROKE_SELECT;
			break;
		}
	}
}

/* -------- GP-Frame API ---------- */

/* delete the last stroke of the given frame */
void BKE_gpencil_frame_delete_laststroke(bGPDlayer *gpl, bGPDframe *gpf)
{
	bGPDstroke *gps = (gpf) ? gpf->strokes.last : NULL;
	int cfra = (gpf) ? gpf->framenum : 0; /* assume that the current frame was not locked */
	
	/* error checking */
	if (ELEM(NULL, gpf, gps))
		return;
	
	/* free the stroke and its data */
	MEM_freeN(gps->points);
	MEM_freeN(gps->triangles);
	BLI_freelinkN(&gpf->strokes, gps);
	
	/* if frame has no strokes after this, delete it */
	if (BLI_listbase_is_empty(&gpf->strokes)) {
		BKE_gpencil_layer_delframe(gpl, gpf);
		BKE_gpencil_layer_getframe(gpl, cfra, 0);
	}
}

/* -------- GP-Layer API ---------- */

/* Check if the given layer is able to be edited or not */
bool gpencil_layer_is_editable(const bGPDlayer *gpl)
{
	/* Sanity check */
	if (gpl == NULL)
		return false;
	
	/* Layer must be: Visible + Editable */
	if ((gpl->flag & (GP_LAYER_HIDE | GP_LAYER_LOCKED)) == 0) {
		/* Opacity must be sufficiently high that it is still "visible"
		 * Otherwise, it's not really "visible" to the user, so no point editing...
		 */
		if (gpl->opacity > GPENCIL_ALPHA_OPACITY_THRESH) {
			return true;
		}
	}
	
	/* Something failed */
	return false;
}

/* Look up the gp-frame on the requested frame number, but don't add a new one */
bGPDframe *BKE_gpencil_layer_find_frame(bGPDlayer *gpl, int cframe)
{
	bGPDframe *gpf;
	
	/* Search in reverse order, since this is often used for playback/adding,
	 * where it's less likely that we're interested in the earlier frames
	 */
	for (gpf = gpl->frames.last; gpf; gpf = gpf->prev) {
		if (gpf->framenum == cframe) {
			return gpf;
		}
	}
	
	return NULL;
}

/* get the appropriate gp-frame from a given layer
 *	- this sets the layer's actframe var (if allowed to)
 *	- extension beyond range (if first gp-frame is after all frame in interest and cannot add)
 */
bGPDframe *BKE_gpencil_layer_getframe(bGPDlayer *gpl, int cframe, eGP_GetFrame_Mode addnew)
{
	bGPDframe *gpf = NULL;
	short found = 0;
	
	/* error checking */
	if (gpl == NULL) return NULL;
	
	/* check if there is already an active frame */
	if (gpl->actframe) {
		gpf = gpl->actframe;
		
		/* do not allow any changes to layer's active frame if layer is locked from changes
		 * or if the layer has been set to stay on the current frame
		 */
		if (gpl->flag & GP_LAYER_FRAMELOCK)
			return gpf;
		/* do not allow any changes to actframe if frame has painting tag attached to it */
		if (gpf->flag & GP_FRAME_PAINT) 
			return gpf;
		
		/* try to find matching frame */
		if (gpf->framenum < cframe) {
			for (; gpf; gpf = gpf->next) {
				if (gpf->framenum == cframe) {
					found = 1;
					break;
				}
				else if ((gpf->next) && (gpf->next->framenum > cframe)) {
					found = 1;
					break;
				}
			}
			
			/* set the appropriate frame */
			if (addnew) {
				if ((found) && (gpf->framenum == cframe))
					gpl->actframe = gpf;
				else if (addnew == GP_GETFRAME_ADD_COPY)
					gpl->actframe = BKE_gpencil_frame_addcopy(gpl, cframe);
				else
					gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
			}
			else if (found)
				gpl->actframe = gpf;
			else
				gpl->actframe = gpl->frames.last;
		}
		else {
			for (; gpf; gpf = gpf->prev) {
				if (gpf->framenum <= cframe) {
					found = 1;
					break;
				}
			}
			
			/* set the appropriate frame */
			if (addnew) {
				if ((found) && (gpf->framenum == cframe))
					gpl->actframe = gpf;
				else if (addnew == GP_GETFRAME_ADD_COPY)
					gpl->actframe = BKE_gpencil_frame_addcopy(gpl, cframe);
				else
					gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
			}
			else if (found)
				gpl->actframe = gpf;
			else
				gpl->actframe = gpl->frames.first;
		}
	}
	else if (gpl->frames.first) {
		/* check which of the ends to start checking from */
		const int first = ((bGPDframe *)(gpl->frames.first))->framenum;
		const int last = ((bGPDframe *)(gpl->frames.last))->framenum;
		
		if (abs(cframe - first) > abs(cframe - last)) {
			/* find gp-frame which is less than or equal to cframe */
			for (gpf = gpl->frames.last; gpf; gpf = gpf->prev) {
				if (gpf->framenum <= cframe) {
					found = 1;
					break;
				}
			}
		}
		else {
			/* find gp-frame which is less than or equal to cframe */
			for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
				if (gpf->framenum <= cframe) {
					found = 1;
					break;
				}
			}
		}
		
		/* set the appropriate frame */
		if (addnew) {
			if ((found) && (gpf->framenum == cframe))
				gpl->actframe = gpf;
			else
				gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
		}
		else if (found)
			gpl->actframe = gpf;
		else {
			/* unresolved errogenous situation! */
			printf("Error: cannot find appropriate gp-frame\n");
			/* gpl->actframe should still be NULL */
		}
	}
	else {
		/* currently no frames (add if allowed to) */
		if (addnew)
			gpl->actframe = BKE_gpencil_frame_addnew(gpl, cframe);
		else {
			/* don't do anything... this may be when no frames yet! */
			/* gpl->actframe should still be NULL */
		}
	}
	
	/* return */
	return gpl->actframe;
}

/* delete the given frame from a layer */
bool BKE_gpencil_layer_delframe(bGPDlayer *gpl, bGPDframe *gpf)
{
	bool changed = false;
	
	/* error checking */
	if (ELEM(NULL, gpl, gpf))
		return false;
	
	/* if this frame was active, make the previous frame active instead 
	 * since it's tricky to set active frame otherwise
	 */
	if (gpl->actframe == gpf)
		gpl->actframe = gpf->prev;
	else
		gpl->actframe = NULL;
	
	/* free the frame and its data */
	changed = BKE_gpencil_free_strokes(gpf);
	BLI_freelinkN(&gpl->frames, gpf);
	
	return changed;
}

/* get the active gp-layer for editing */
bGPDlayer *BKE_gpencil_layer_getactive(bGPdata *gpd)
{
	bGPDlayer *gpl;
	
	/* error checking */
	if (ELEM(NULL, gpd, gpd->layers.first))
		return NULL;
		
	/* loop over layers until found (assume only one active) */
	for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		if (gpl->flag & GP_LAYER_ACTIVE)
			return gpl;
	}
	
	/* no active layer found */
	return NULL;
}

/* set the active gp-layer */
void BKE_gpencil_layer_setactive(bGPdata *gpd, bGPDlayer *active)
{
	bGPDlayer *gpl;
	
	/* error checking */
	if (ELEM(NULL, gpd, gpd->layers.first, active))
		return;
		
	/* loop over layers deactivating all */
	for (gpl = gpd->layers.first; gpl; gpl = gpl->next)
		gpl->flag &= ~GP_LAYER_ACTIVE;
	
	/* set as active one */
	active->flag |= GP_LAYER_ACTIVE;
}

/* delete the active gp-layer */
void BKE_gpencil_layer_delete(bGPdata *gpd, bGPDlayer *gpl)
{
	/* error checking */
	if (ELEM(NULL, gpd, gpl)) 
		return;
	
	/* free layer */
	BKE_gpencil_free_frames(gpl);
	BLI_freelinkN(&gpd->layers, gpl);
}

/* ************************************************** */
/* get the active gp-brush for editing */
bGPDbrush *BKE_gpencil_brush_getactive(ToolSettings *ts)
{
	bGPDbrush *brush;

	/* error checking */
	if (ELEM(NULL, ts, ts->gp_brushes.first)) {
		return NULL;
	}

	/* loop over brushes until found (assume only one active) */
	for (brush = ts->gp_brushes.first; brush; brush = brush->next) {
		if (brush->flag & GP_BRUSH_ACTIVE) {
			return brush;
		}
	}

	/* no active brush found */
	return NULL;
}

/* set the active gp-brush */
void BKE_gpencil_brush_setactive(ToolSettings *ts, bGPDbrush *active)
{
	bGPDbrush *brush;

	/* error checking */
	if (ELEM(NULL, ts, ts->gp_brushes.first, active)) {
		return;
	}

	/* loop over brushes deactivating all */
	for (brush = ts->gp_brushes.first; brush; brush = brush->next) {
		brush->flag &= ~GP_BRUSH_ACTIVE;
	}

	/* set as active one */
	active->flag |= GP_BRUSH_ACTIVE;
}

/* delete the active gp-brush */
void BKE_gpencil_brush_delete(ToolSettings *ts, bGPDbrush *brush)
{
	/* error checking */
	if (ELEM(NULL, ts, brush)) {
		return;
	}

	/* free curves */
	if (brush->cur_sensitivity) {
		curvemapping_free(brush->cur_sensitivity);
	}
	if (brush->cur_strength) {
		curvemapping_free(brush->cur_strength);
	}
	if (brush->cur_jitter) {
		curvemapping_free(brush->cur_jitter);
	}

	/* free */
	BLI_freelinkN(&ts->gp_brushes, brush);
}

/* ************************************************** */
/* get the active gp-palette for editing */
bGPDpalette *BKE_gpencil_palette_getactive(bGPdata *gpd)
{
	bGPDpalette *palette;

	/* error checking */
	if (ELEM(NULL, gpd, gpd->palettes.first)) {
		return NULL;
	}

	/* loop over palettes until found (assume only one active) */
	for (palette = gpd->palettes.first; palette; palette = palette->next) {
		if (palette->flag & PL_PALETTE_ACTIVE)
			return palette;
	}

	/* no active palette found */
	return NULL;
}

/* set the active gp-palette */
void BKE_gpencil_palette_setactive(bGPdata *gpd, bGPDpalette *active)
{
	bGPDpalette *palette;

	/* error checking */
	if (ELEM(NULL, gpd, gpd->palettes.first, active)) {
		return;
	}

	/* loop over palettes deactivating all */
	for (palette = gpd->palettes.first; palette; palette = palette->next) {
		palette->flag &= ~PL_PALETTE_ACTIVE;
	}

	/* set as active one */
	active->flag |= PL_PALETTE_ACTIVE;
	/* force color recalc */
	BKE_gpencil_palette_change_strokes(gpd);
}

/* delete the active gp-palette */
void BKE_gpencil_palette_delete(bGPdata *gpd, bGPDpalette *palette)
{
	/* error checking */
	if (ELEM(NULL, gpd, palette)) {
		return;
	}

	/* free colors */
	free_gpencil_colors(palette);
	BLI_freelinkN(&gpd->palettes, palette);
	/* force color recalc */
	BKE_gpencil_palette_change_strokes(gpd);
}

/* Set all strokes to recalc the palette color */
void BKE_gpencil_palette_change_strokes(bGPdata *gpd)
{
	bGPDlayer *gpl;
	bGPDframe *gpf;
	bGPDstroke *gps;

	for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
			for (gps = gpf->strokes.first; gps; gps = gps->next) {
				gps->flag |= GP_STROKE_RECALC_COLOR;
			}
		}
	}
}


/* get the active gp-palettecolor for editing */
bGPDpalettecolor *BKE_gpencil_palettecolor_getactive(bGPDpalette *palette)
{
	bGPDpalettecolor *palcolor;

	/* error checking */
	if (ELEM(NULL, palette, palette->colors.first)) {
		return NULL;
	}

	/* loop over colors until found (assume only one active) */
	for (palcolor = palette->colors.first; palcolor; palcolor = palcolor->next) {
		if (palcolor->flag & PC_COLOR_ACTIVE) {
			return palcolor;
		}
	}

	/* no active color found */
	return NULL;
}
/* get the gp-palettecolor looking for name */
bGPDpalettecolor *BKE_gpencil_palettecolor_getbyname(bGPDpalette *palette, char *name)
{
	/* error checking */
	if (ELEM(NULL, palette, name)) {
		return NULL;
	}

	return BLI_findstring(&palette->colors, name, offsetof(bGPDpalettecolor, info));
}

/* Change color name in all gpd datablocks */
void BKE_gpencil_palettecolor_allnames(PaletteColor *palcolor, const char *newname)
{
	bGPdata *gpd;
	Main *bmain = G.main;

	for (gpd = bmain->gpencil.first; gpd; gpd = gpd->id.next) {
		BKE_gpencil_palettecolor_changename(palcolor, gpd, newname);
	}
}

/* Change color name in all strokes */
void BKE_gpencil_palettecolor_changename(PaletteColor *palcolor, bGPdata *gpd, const char *newname)
{
	bGPDlayer *gpl;
	bGPDframe *gpf;
	bGPDstroke *gps;
	
	/* Sanity checks (gpd may not be set in the RNA pointers sometimes) */
	if (ELEM(NULL, gpd, newname))
		return;
	
	for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
			for (gps = gpf->strokes.first; gps; gps = gps->next) {
				if (gps->palcolor == palcolor) {
					BLI_strncpy(gps->colorname, newname, sizeof(gps->colorname));
				}
			}
		}
	}
		
}

/* Delete all strokes of the color for all gpd datablocks */
void BKE_gpencil_palettecolor_delete_allstrokes(PaletteColor *palcolor)
{
	bGPdata *gpd;
	bGPDlayer *gpl;
	bGPDframe *gpf;
	bGPDstroke *gps, *gpsn;

	Main *bmain = G.main;

	for (gpd = bmain->gpencil.first; gpd; gpd = gpd->id.next) {
		for (gpl = gpd->layers.first; gpl; gpl = gpl->next) {
			for (gpf = gpl->frames.first; gpf; gpf = gpf->next) {
				for (gps = gpf->strokes.first; gps; gps = gpsn) {
					gpsn = gps->next;

					if (gps->palcolor == palcolor) {
						if (gps->points) MEM_freeN(gps->points);
						if (gps->triangles) MEM_freeN(gps->triangles);
						BLI_freelinkN(&gpf->strokes, gps);
					}
				}
			}
		}
		BKE_gpencil_batch_cache_dirty(gpd);
	}
}

/* set the active gp-palettecolor */
void BKE_gpencil_palettecolor_setactive(bGPDpalette *palette, bGPDpalettecolor *active)
{
	bGPDpalettecolor *palcolor;

	/* error checking */
	if (ELEM(NULL, palette, palette->colors.first, active)) {
		return;
	}

	/* loop over colors deactivating all */
	for (palcolor = palette->colors.first; palcolor; palcolor = palcolor->next) {
		palcolor->flag &= ~PC_COLOR_ACTIVE;
	}

	/* set as active one */
	active->flag |= PC_COLOR_ACTIVE;
}

/* delete the active gp-palettecolor */
void BKE_gpencil_palettecolor_delete(bGPDpalette *palette, bGPDpalettecolor *palcolor)
{
	/* error checking */
	if (ELEM(NULL, palette, palcolor)) {
		return;
	}

	/* free */
	BLI_freelinkN(&palette->colors, palcolor);
}

/**
* Helper heuristic for determining if a path is compatible with the basepath
*
* \param path Full RNA-path from some data (usually an F-Curve) to compare
* \param basepath Shorter path fragment to look for
* \return Whether there is a match
*/
static bool UNUSED_FUNCTION(gp_animpath_matches_basepath)(const char path[], const char basepath[])
{
	/* we need start of path to be basepath */
	return (path && basepath) && STRPREFIX(path, basepath);
}

/* Transfer the animation data from bGPDpalette to Palette */
void BKE_gpencil_move_animdata_to_palettes(bGPdata *gpd)
{
	Main *bmain = G.main;
	Palette *palette = NULL;
	AnimData *srcAdt = NULL, *dstAdt = NULL;
	FCurve *fcu = NULL;
	char info[64];

	/* sanity checks */
	if (ELEM(NULL, gpd)) {
		if (G.debug & G_DEBUG)
			printf("ERROR: no source ID to separate AnimData with\n");
		return;
	}
	/* get animdata from src, and create for destination (if needed) */
	srcAdt = BKE_animdata_from_id((ID *)gpd);
	if (ELEM(NULL, srcAdt)) {
		if (G.debug & G_DEBUG)
			printf("ERROR: no source AnimData\n");
		return;
	}

	/* find first palette */
	for (fcu = srcAdt->action->curves.first; fcu; fcu = fcu->next) {
		if (strncmp("palette", fcu->rna_path, 7) == 0) {
			int x = strcspn(fcu->rna_path, "[") + 2;
			int y = strcspn(fcu->rna_path, "]");
			BLI_strncpy(info, fcu->rna_path + x, y - x);
			palette = BLI_findstring(&bmain->palettes, info, offsetof(ID, name) + 2);
			break;
		}
	}
	if (ELEM(NULL, palette)) {
		if (G.debug & G_DEBUG)
			printf("ERROR: Palette %s not found\n", info);
		return;
	}

	/* active action */
	if (srcAdt->action) {
		/* get animdata from destination or create (if needed) */
		dstAdt = BKE_animdata_add_id((ID *) palette);
		if (ELEM(NULL, dstAdt)) {
			if (G.debug & G_DEBUG)
				printf("ERROR: no AnimData for destination palette\n");
			return;
		}

		/* create destination action */
		dstAdt->action = add_empty_action(G.main, srcAdt->action->id.name + 2);
		/* move fcurves */
		action_move_fcurves_by_basepath(srcAdt->action, dstAdt->action, "palettes");

		/* loop over base paths, to fix for each one... */
		for (fcu = dstAdt->action->curves.first; fcu; fcu = fcu->next) {
			if (strncmp("palette", fcu->rna_path, 7) == 0) {
				int x = strcspn(fcu->rna_path, ".") + 1;
				BLI_strncpy(fcu->rna_path, fcu->rna_path + x, strlen(fcu->rna_path));
			}
		}
	}
}

/* Change draw manager status in all gpd datablocks */
void BKE_gpencil_batch_cache_alldirty()
{
	bGPdata *gpd;
	Main *bmain = G.main;

	for (gpd = bmain->gpencil.first; gpd; gpd = gpd->id.next) {
		BKE_gpencil_batch_cache_dirty(gpd);
	}
}

/* get stroke min max values */
void static gpencil_minmax(bGPdata *gpd, float min[3], float max[3])
{
	int i;
	bGPDspoint *pt;
	bGPDframe *gpf;

	for (bGPDlayer *gpl = gpd->layers.first; gpl; gpl = gpl->next) {
		gpf= gpl->actframe;
		if (!gpf) {
			continue;
		}
		for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
			for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
				/* min */
				if (pt->x < min[0]) {
					min[0] = pt->x;
				}
				if (pt->y < min[1]) {
					min[1] = pt->y;
				}
				if (pt->z < min[2]) {
					min[2] = pt->z;
				}
				/* max */
				if (pt->x > max[0]) {
					max[0] = pt->x;
				}
				if (pt->y > max[1]) {
					max[1] = pt->y;
				}
				if (pt->z > max[2]) {
					max[2] = pt->z;
				}
			}
		}
	}
}

/* create bounding box values */
static void boundbox_gpencil(Object *ob)
{
	BoundBox *bb;
	bGPdata *gpd;
	float min[3], max[3];

	if (ob->bb == NULL) {
		ob->bb = MEM_callocN(sizeof(BoundBox), "GPencil boundbox");
	}

	bb = ob->bb;
	gpd= ob->gpd;

	INIT_MINMAX(min, max);
	gpencil_minmax(gpd, min, max);
	BKE_boundbox_init_from_minmax(bb, min, max);

	bb->flag &= ~BOUNDBOX_DIRTY;
}

/* get bounding box */
BoundBox *BKE_gpencil_boundbox_get(Object *ob)
{
	if ((!ob) || (ob->gpd == NULL))
		return NULL;

	if ((ob->bb) && ((ob->bb->flag & BOUNDBOX_DIRTY) == 0) && ((ob->gpd->flag & GP_DATA_CACHE_IS_DIRTY) == 0))
		return ob->bb;

	boundbox_gpencil(ob);

	return ob->bb;
}

/********************  Modifiers **********************************/
/* calculate stroke normal using some points */
static void ED_gpencil_stroke_normal(const bGPDstroke *gps, float r_normal[3])
{
	if (gps->totpoints < 3) {
		zero_v3(r_normal);
		return;
	}

	bGPDspoint *points = gps->points;
	int totpoints = gps->totpoints;

	const bGPDspoint *pt0 = &points[0];
	const bGPDspoint *pt1 = &points[1];
	const bGPDspoint *pt3 = &points[(int)(totpoints * 0.75)];

	float vec1[3];
	float vec2[3];

	/* initial vector (p0 -> p1) */
	sub_v3_v3v3(vec1, &pt1->x, &pt0->x);

	/* point vector at 3/4 */
	sub_v3_v3v3(vec2, &pt3->x, &pt0->x);

	/* vector orthogonal to polygon plane */
	cross_v3_v3v3(r_normal, vec1, vec2);

	/* Normalize vector */
	normalize_v3(r_normal);
}

/* calculate a noise base on stroke direction */
void ED_gpencil_noise_modifier(GpencilNoiseModifierData *mmd, bGPDlayer *gpl, bGPDstroke *gps)
{
	bGPDspoint *pt0, *pt1;
	float shift, vran, vdir;
	float normal[3];
	float vec1[3], vec2[3];

	/* Need three points or more */
	if (gps->totpoints < 3) {
		return;
	}
	/* omit if filter by layer */
	if (mmd->layername[0] != '\0') {
		if (!STREQ(mmd->layername, gpl->info)) {
			return;
		}
	}
	/* verify pass */
	if (gps->palcolor->index != mmd->passindex) {
		return;
	}

	/* calculate stroke normal*/
	ED_gpencil_stroke_normal(gps, normal);

	/* move points (starting in point 2) */
	for (int i = 1; i < gps->totpoints - 1; i++) {
		pt0 = &gps->points[i - 1];
		pt1 = &gps->points[i];
		/* initial vector (p0 -> p1) */
		sub_v3_v3v3(vec1, &pt1->x, &pt0->x);
		vran = len_v3(vec1);
		/* vector orthogonal to normal */
		cross_v3_v3v3(vec2, vec1, normal);
		normalize_v3(vec2);
		/* use random noise */
		if (mmd->flag & GP_NOISE_USE_RANDOM) {
			vran = BLI_frand();
			vdir = BLI_frand();
		}
		else {
			vran = 1.0f;
			vdir = i % 2;
		}

		/* apply randomness to location of the point */
		if (mmd->flag & GP_NOISE_MOD_LOCATION) {
			/* factor is too sensitive, so need divide */
			shift = vran * mmd->factor / 10.0f;
			if (vdir > 0.5f) {
				mul_v3_fl(vec2, shift);
			}
			else {
				mul_v3_fl(vec2, shift * -1.0f);
			}
			add_v3_v3(&pt1->x, vec2);
		}

		/* apply randomness to thickness */
		if (mmd->flag & GP_NOISE_MOD_THICKNESS) {
			if (vdir > 0.5f) {
				pt1->pressure -= pt1->pressure * vran * mmd->factor;
			}
			else {
				pt1->pressure += pt1->pressure * vran * mmd->factor;;
			}
			CLAMP_MIN(pt1->pressure, GPENCIL_STRENGTH_MIN);
		}

		/* apply randomness to color strength */
		if (mmd->flag & GP_NOISE_MOD_STRENGTH) {
			if (vdir > 0.5f) {
				pt1->strength -= pt1->strength * vran * mmd->factor;
			}
			else {
				pt1->strength += pt1->strength * vran * mmd->factor;
			}
			CLAMP_MIN(pt1->strength, GPENCIL_STRENGTH_MIN);
		}
	}
}

/* apply stroke modifiers */
void ED_gpencil_stroke_modifiers(Object *ob, bGPDlayer *gpl, bGPDstroke *gps)
{
	ModifierData *md;

	for (md = ob->modifiers.first; md; md = md->next) {
		if (((md->mode & eModifierMode_Realtime) && ((G.f & G_RENDER_OGL) == 0)) ||
			((md->mode & eModifierMode_Render) && (G.f & G_RENDER_OGL))) {
			switch (md->type) {
				// Noise Modifier
			case eModifierType_GpencilNoise:
				ED_gpencil_noise_modifier((GpencilNoiseModifierData *)md, gpl, gps);
				break;
			default:
				break;
			}
		}
	}
}
