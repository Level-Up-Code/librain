/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#include <stddef.h>
#include <string.h>

#include <GL/glew.h>

#include <XPLMDisplay.h>
#include <XPLMGraphics.h>

#include <acfutils/assert.h>
#include <acfutils/crc64.h>
#include <acfutils/dr.h>
#include <acfutils/math.h>
#include <acfutils/perf.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/shader.h>
#include <acfutils/time.h>

#include "librain.h"

#if 0
#define	WS_TEMP_DEBUG
#endif

#define	DESTROY_OP(var, zero_val, destroy_op) \
	do { \
		if ((var) != (zero_val)) { \
			destroy_op; \
			(var) = (zero_val); \
		} \
	} while (0)
#define	RAIN_DRAW_TIMEOUT	15	/* seconds */

typedef enum {
	PANEL_RENDER_TYPE_2D = 0,
	PANEL_RENDER_TYPE_3D_UNLIT = 1,
	PANEL_RENDER_TYPE_3D_LIT = 2
} panel_render_type_t;

static bool_t	inited = B_FALSE;
static char	*pluginpath = NULL;

static GLuint	screenshot_tex = 0;
static GLuint	screenshot_fbo = 0;
static GLint	old_vp[4] = { -1, -1, -1, -1 };
static float	last_rain_t = 0;

static GLint	ws_temp_prog = 0;
static GLint	rain_stage1_prog = 0;
static GLint	rain_stage2_prog = 0;
static GLint	ws_rain_prog = 0;
static GLint	ws_smudge_prog = 0;
#ifdef	WS_TEMP_DEBUG
static GLint	ws_temp_debug_prog = 0;
#endif

static GLint	saved_vp[4] = { -1, -1, -1, -1 };
static bool_t	prepare_ran = B_TRUE;

typedef struct {
	const librain_glass_t	*glass;

	float			last_stage1_t;

#define	WATER_DEPTH_TEX_W	1024
#define	WATER_DEPTH_TEX_H	1024
	int			water_depth_cur;
	GLuint			water_depth_tex[2];
	GLuint			water_depth_fbo[2];

#define	WATER_NORM_TEX_W	1024
#define	WATER_NORM_TEX_H	1024
	GLuint			water_norm_tex;
	GLuint			water_norm_fbo;

#define	WS_TEMP_TEX_W		256
#define	WS_TEMP_TEX_H		256
#define	WS_TEMP_COMP_INTVAL	(1.0 / 15.0)	/* 20 fps */
	float			last_ws_temp_t;
	GLuint			ws_temp_tex[2];
	GLuint			ws_temp_fbo[2];
	int			ws_temp_cur;

	GLuint			ws_smudge_tex;
	GLuint			ws_smudge_fbo;

	vect2_t			gp;
	vect2_t			tp;
	vect2_t			wp;
	float			thrust;
	float			wind;
} glass_info_t;

static glass_info_t *glass_infos = NULL;
static size_t num_glass_infos = 0;

static struct {
	dr_t	panel_render_type;
	dr_t	sim_time;
	dr_t	proj_matrix;
	dr_t	acf_matrix;
	dr_t	viewport;
	dr_t	precip_rat;
	dr_t	prop_thrust;
	dr_t	hdg;
	dr_t	beta;
	dr_t	gs;
	dr_t	wind_dir;
	dr_t	wind_spd;
	dr_t	rot_rate;
	dr_t	le_temp;
} drs;

#define	RAIN_COMP_PHASE		xplm_Phase_Gauges
#define	RAIN_COMP_BEFORE	0

static void
setup_texture(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data)
{
	XPLMBindTexture2d(tex, GL_TEXTURE_2D);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width, height, 0, format,
	    type, data);
}

static void
setup_color_fbo_for_tex(GLuint fbo, GLuint tex)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex, 0);
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);
}

static void
update_vectors(glass_info_t *gi)
{
	float rot_rate = dr_getf(&drs.rot_rate);
	double wind_spd = KT2MPS(dr_getf(&drs.wind_spd));
	double gs = dr_getf(&drs.gs);
	vect2_t wind_comp = VECT2(0, wind_spd);
	vect2_t gs_comp = VECT2(0, gs);
	vect2_t total_wind;
	vect2_t thrust_v;

	gi->gp = vect2_scmul(gi->glass->gravity_point, WATER_DEPTH_TEX_W);
	gi->tp = vect2_scmul(gi->glass->thrust_point, WATER_DEPTH_TEX_W);
	gi->wp = vect2_scmul(gi->glass->wind_point, WATER_DEPTH_TEX_W);

	wind_comp = vect2_rot(wind_comp,
	    normalize_hdg(dr_getf(&drs.wind_dir)) - dr_getf(&drs.hdg));
	wind_comp = vect2_scmul(wind_comp, gi->glass->wind_factor);

	gs_comp = vect2_rot(gs_comp, dr_getf(&drs.beta));
	total_wind = vect2_add(wind_comp, gs_comp);

	total_wind = vect2_scmul(total_wind, 1.0 / gi->glass->max_tas);

	thrust_v = vect2_scmul(VECT2(0,
	    MAX(dr_getf(&drs.prop_thrust) / gi->glass->max_thrust, 0)),
	    gi->glass->thrust_factor);

	gi->wind = clamp(vect2_abs(total_wind), 0, 1);

	gi->thrust = clamp(vect2_abs(thrust_v), 0, 1);
	if (vect2_abs(total_wind) > 0.001)
		gi->wp = vect2_rot(gi->wp, dir2hdg(total_wind));

	gi->gp = vect2_rot(gi->gp, clamp(-2 * rot_rate, -30, 30));
}

static void
update_viewport(void)
{
	GLint vp[4];

	glGetIntegerv(GL_VIEWPORT, vp);
	if (old_vp[2] == vp[2] && old_vp[3] == vp[3])
		return;

	/* If the viewport size has changed, update the textures. */
	memcpy(old_vp, vp, sizeof (vp));
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, vp[2], vp[3], 0,
	    GL_RGB, GL_UNSIGNED_BYTE, NULL);

	for (size_t i = 0; i < num_glass_infos; i++) {
		XPLMBindTexture2d(glass_infos[i].ws_smudge_tex, GL_TEXTURE_2D);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vp[2], vp[3], 0,
		    GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
}

static void
grab_screenshot(void)
{
	GLint old_fbo;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, screenshot_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(0, 0, old_vp[2], old_vp[3],
	    0, 0, old_vp[2], old_vp[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
}

void
librain_draw_z_depth(const obj8_t *obj, const char **z_depth_group_ids)
{
	if (!prepare_ran)
		return;
	XPLMSetGraphicsState(0, 0, 0, 1, 1, 1, 1);
	glColor3f(1, 1, 1);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	for (int i = 0; z_depth_group_ids[i] != NULL; i++)
		obj8_draw_group(obj, z_depth_group_ids[i]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void
ws_temp_comp(glass_info_t *gi)
{
	float now = dr_getf(&drs.sim_time);
	float d_t = now - gi->last_ws_temp_t;
	float rand_seed = (crc64_rand() % 1000000) / 1000000.0;
	GLint old_fbo;

	if (d_t < WS_TEMP_COMP_INTVAL)
		return;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
	    gi->ws_temp_fbo[!gi->ws_temp_cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glUseProgram(ws_temp_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_temp_prog, "src"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->water_depth_tex[gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_temp_prog, "depth"), 0);

	glUniform1f(glGetUniformLocation(ws_temp_prog, "rand_seed"),
	    rand_seed);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "le_temp"),
	    C2KELVIN(dr_getf(&drs.le_temp)));
	glUniform1f(glGetUniformLocation(ws_temp_prog, "cabin_temp"),
	    gi->glass->cabin_temp);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "wind_fact"),
	    MAX(gi->wind, gi->thrust));
	glUniform1f(glGetUniformLocation(ws_temp_prog, "d_t"), d_t);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "inertia_in"),
	    gi->glass->therm_inertia);
	glUniform4fv(glGetUniformLocation(ws_temp_prog, "heat_zones"),
	    16, gi->glass->heat_zones);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "heat_tgt_temps"),
	    4, gi->glass->heat_tgt_temps);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "precip_intens"),
	    dr_getf(&drs.precip_rat));
	glUniform2fv(glGetUniformLocation(ws_temp_prog, "hot_air_src"),
	    4, gi->glass->hot_air_src);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "hot_air_radius"),
	    2, gi->glass->hot_air_radius);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "hot_air_temp"),
	    2, gi->glass->hot_air_temp);

	glBegin(GL_QUADS);
	glVertex2f(0, 0);
	glVertex2f(0, WS_TEMP_TEX_H);
	glVertex2f(WS_TEMP_TEX_W, WS_TEMP_TEX_H);
	glVertex2f(WS_TEMP_TEX_W, 0);
	glEnd();

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	gi->ws_temp_cur = !gi->ws_temp_cur;
	gi->last_ws_temp_t = now;
}

static void
rain_stage1_comp(glass_info_t *gi)
{
	GLint old_fbo;
	float rand_seed = (crc64_rand() % 1000000) / 1000000.0;
	float cur_t = dr_getf(&drs.sim_time);
	float d_t = cur_t - gi->last_stage1_t;

	if (gi->last_stage1_t == 0.0)
		gi->last_stage1_t = cur_t;
	d_t = cur_t - gi->last_stage1_t;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
	    gi->water_depth_fbo[!gi->water_depth_cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(rain_stage1_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_depth_tex[gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage1_prog, "tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage1_prog, "temp_tex"), 1);

	glUniform1f(glGetUniformLocation(rain_stage1_prog, "rand_seed"),
	    rand_seed);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "precip_intens"),
	    pow(dr_getf(&drs.precip_rat), 1.1));
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "thrust"),
	    gi->thrust);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "wind"), gi->wind);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "gravity"),
	    (float)gi->glass->gravity_factor);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "d_t"), d_t);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "gp"),
	    gi->gp.x, gi->gp.y);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "tp"),
	    gi->tp.x, gi->tp.y);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "wp"),
	    gi->wp.x, gi->wp.y);

	glBegin(GL_QUADS);
	glVertex2f(0, 0);
	glVertex2f(0, WATER_DEPTH_TEX_H);
	glVertex2f(WATER_DEPTH_TEX_W, WATER_DEPTH_TEX_H);
	glVertex2f(WATER_DEPTH_TEX_W, 0);
	glEnd();

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	gi->last_stage1_t = cur_t;
}

static void
rain_stage2_comp(glass_info_t *gi)
{
	GLint old_fbo;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gi->water_norm_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(rain_stage2_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage2_prog, "tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage2_prog, "temp_tex"), 1);

	glUniform2f(glGetUniformLocation(rain_stage2_prog, "my_tex_sz"),
	    WATER_NORM_TEX_W, WATER_NORM_TEX_H);
	glUniform2f(glGetUniformLocation(rain_stage2_prog, "tp"),
	    gi->tp.x, gi->tp.y);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "thrust"),
	    gi->thrust);
	glUniform2f(glGetUniformLocation(rain_stage2_prog, "wp"),
	    gi->wp.x, gi->wp.y);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "wind"), gi->wind);

	glBegin(GL_QUADS);
	glVertex2f(0, 0);
	glVertex2f(0, WATER_NORM_TEX_H);
	glVertex2f(WATER_NORM_TEX_W, WATER_NORM_TEX_H);
	glVertex2f(WATER_NORM_TEX_W, 0);
	glEnd();

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
}

int
rain_comp_cb(XPLMDrawingPhase phase, int before, void *refcon)
{
	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	/* Make sure we only run the computations once per frame */
	if (dr_geti(&drs.panel_render_type) != PANEL_RENDER_TYPE_3D_UNLIT)
		return (1);

	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];

		update_vectors(gi);
		ws_temp_comp(gi);
		rain_stage1_comp(gi);
		rain_stage2_comp(gi);
		gi->water_depth_cur = !gi->water_depth_cur;
	}

	return (1);
}

static void
draw_ws_effects(glass_info_t *gi)
{
	GLint old_fbo;

	/*
	 * Pre-stage: render the actual image to a side buffer, but without
	 * any smudging.
	 */
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gi->ws_smudge_fbo);
	glClear(GL_COLOR_BUFFER_BIT);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glUseProgram(ws_rain_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_norm_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "norm_tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "screenshot_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "depth_tex"), 2);

	for (int i = 0; gi->glass->group_ids[i] != NULL; i++)
		obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i]);

	/*
	 * Final stage: render the prepped displaced texture and apply
	 * variable smudging based on water depth.
	 */
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

#if	!defined(WS_TEMP_DEBUG)
	glUseProgram(ws_smudge_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "screenshot_tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_smudge_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "ws_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "depth_tex"), 2);
#else	/* defined(WS_TEMP_DEBUG) */
	glUseProgram(ws_temp_debug_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_temp_debug_prog, "src"), 0);
#endif	/* defined(WS_TEMP_DEBUG) */

	for (int i = 0; gi->glass->group_ids[i] != NULL; i++)
		obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i]);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
}

void
librain_draw_prepare(bool_t force)
{
	GLfloat acf_matrix[16], proj_matrix[16];
	GLint vp[4];
	int w, h;
	double now = dr_getf(&drs.sim_time);

	if (dr_getf(&drs.precip_rat) > 0)
		last_rain_t = now;

	if (now - last_rain_t > RAIN_DRAW_TIMEOUT && !force) {
		prepare_ran = B_FALSE;
		return;
	}
	prepare_ran = B_TRUE;

	update_viewport();
	grab_screenshot();

	glGetIntegerv(GL_VIEWPORT, vp);
	XPLMGetScreenSize(&w, &h);
	if (vp[3] != h) {
		memcpy(saved_vp, vp, sizeof (saved_vp));
		glViewport(vp[0], vp[1], vp[2], h);
	}

	VERIFY3S(dr_getvf32(&drs.acf_matrix, acf_matrix, 0, 16), ==, 16);
	VERIFY3S(dr_getvf32(&drs.proj_matrix, proj_matrix, 0, 16), ==, 16);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadMatrixf(acf_matrix);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	for (int i = 12; i < 16; i++)
		proj_matrix[i] /= 100;
	glLoadMatrixf(proj_matrix);
}

void
librain_draw_exec(void)
{
	if (dr_getf(&drs.sim_time) - last_rain_t <= RAIN_DRAW_TIMEOUT) {
		for (size_t i = 0; i < num_glass_infos; i++)
			draw_ws_effects(&glass_infos[i]);
	}
}

void
librain_draw_finish(void)
{
	if (!prepare_ran)
		return;

	if (saved_vp[0] != -1) {
		glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]);
		saved_vp[0] = saved_vp[1] = saved_vp[2] = saved_vp[3] = -1;
	}

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
}

static void
glass_info_init(glass_info_t *gi, const librain_glass_t *glass)
{
	float temp_tex[WS_TEMP_TEX_W * WS_TEMP_TEX_H];
	float le_temp = dr_getf(&drs.le_temp);

	gi->glass = glass;

	/*
	 * Pre-stage: glass heating/cooling simulation.
	 */
	glGenTextures(2, gi->ws_temp_tex);
	glGenFramebuffers(2, gi->ws_temp_fbo);
	for (int i = 0; i < WS_TEMP_TEX_W * WS_TEMP_TEX_H; i++)
		temp_tex[i] = C2KELVIN(le_temp);
	for (int i = 0; i < 2; i++) {
		setup_texture(gi->ws_temp_tex[i], GL_R32F, WS_TEMP_TEX_W,
		    WS_TEMP_TEX_H, GL_RED, GL_FLOAT, temp_tex);
		setup_color_fbo_for_tex(gi->ws_temp_fbo[i],
		    gi->ws_temp_tex[i]);
	}

	/*
	 * Stage 1: computing water depth.
	 */
	glGenTextures(2, gi->water_depth_tex);
	glGenFramebuffers(2, gi->water_depth_fbo);
	for (int i = 0; i < 2; i++) {
		setup_texture(gi->water_depth_tex[i], GL_RED,
		    WATER_DEPTH_TEX_W, WATER_DEPTH_TEX_H, GL_RED,
		    GL_FLOAT, NULL);
		setup_color_fbo_for_tex(gi->water_depth_fbo[i],
		    gi->water_depth_tex[i]);
	}

	/*
	 * Stage 2: computing normals.
	 */
	glGenTextures(1, &gi->water_norm_tex);
	glGenFramebuffers(1, &gi->water_norm_fbo);
	setup_texture(gi->water_norm_tex, GL_RG, WATER_NORM_TEX_W,
	    WATER_NORM_TEX_H, GL_RG, GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(gi->water_norm_fbo, gi->water_norm_tex);

	/*
	 * Final render pre-stage: capture the full res displacement,
	 * which is then used as the final smudge pass.
	 */
	glGenTextures(1, &gi->ws_smudge_tex);
	glGenFramebuffers(1, &gi->ws_smudge_fbo);
	setup_texture(gi->ws_smudge_tex, GL_RGBA, 16, 16, GL_RGBA,
	    GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(gi->ws_smudge_fbo, gi->ws_smudge_tex);
}

static void
water_effects_init(const librain_glass_t *glass, size_t num)
{
	glass_infos = safe_calloc(sizeof (*glass_infos), num);
	num_glass_infos = num;

	for (size_t i = 0; i < num_glass_infos; i++)
		glass_info_init(&glass_infos[i], &glass[i]);
}

static void
water_effects_fini(void)
{
	DESTROY_OP(ws_temp_prog, 0, glDeleteProgram(ws_temp_prog));
	DESTROY_OP(rain_stage1_prog, 0, glDeleteProgram(rain_stage1_prog));
	DESTROY_OP(rain_stage2_prog, 0, glDeleteProgram(rain_stage2_prog));
	DESTROY_OP(ws_rain_prog, 0, glDeleteProgram(ws_rain_prog));
	DESTROY_OP(ws_smudge_prog, 0, glDeleteProgram(ws_smudge_prog));
#ifdef	WS_TEMP_DEBUG
	DESTROY_OP(ws_temp_debug_prog, 0, glDeleteProgram(ws_temp_debug_prog));
#endif

	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];

		DESTROY_OP(gi->water_depth_fbo[0], 0,
		    glDeleteFramebuffers(2, gi->water_depth_fbo));
		DESTROY_OP(gi->water_depth_tex[0], 0,
		    glDeleteTextures(2, gi->water_depth_tex));

		DESTROY_OP(gi->water_norm_fbo, 0,
		    glDeleteFramebuffers(1, &gi->water_norm_fbo));
		DESTROY_OP(gi->water_norm_tex, 0,
		    glDeleteTextures(1, &gi->water_norm_tex));

		DESTROY_OP(gi->ws_temp_fbo[0], 0,
		    glDeleteFramebuffers(2, gi->ws_temp_fbo));
		DESTROY_OP(gi->ws_temp_tex[0], 0,
		    glDeleteTextures(2, gi->ws_temp_tex));

		DESTROY_OP(gi->ws_smudge_fbo, 0,
		    glDeleteFramebuffers(1, &gi->ws_smudge_fbo));
		DESTROY_OP(gi->ws_smudge_tex, 0,
		    glDeleteTextures(1, &gi->ws_smudge_tex));
	}

	free(glass_infos);
	glass_infos = NULL;
	num_glass_infos = 0;
}

static bool_t
reload_gl_prog(GLint *prog, const char *progname, const char *vert_shader,
    const char *frag_shader)
{
	char *path_vert = NULL, *path_frag = NULL;

	if (*prog != 0) {
		glDeleteProgram(*prog);
		*prog = 0;
	}
	if (vert_shader != NULL)
		path_vert = mkpathname(pluginpath, "data", vert_shader, NULL);
	if (frag_shader != NULL)
		path_frag = mkpathname(pluginpath, "data", frag_shader, NULL);
	*prog = shader_prog_from_file(progname, path_vert, path_frag);
	lacf_free(path_vert);
	lacf_free(path_frag);

	return (*prog != 0);
}

bool_t
librain_reload_gl_progs(void)
{
	return (reload_gl_prog(&ws_temp_prog, "ws_temp",
	    NULL, "ws_temp.frag") &&
	    reload_gl_prog(&rain_stage1_prog, "rain_stage1",
	    NULL, "rain_stage1.frag") &&
	    reload_gl_prog(&rain_stage2_prog, "rain_stage2",
	    NULL, "rain_stage2.frag") &&
	    reload_gl_prog(&ws_rain_prog, "ws_rain_prog",
	    "ws_rain.vert", "ws_rain.frag") &&
	    reload_gl_prog(&ws_smudge_prog, "ws_smudge_prog",
	    "ws_smudge.vert", "ws_smudge.frag")
#ifdef	WS_TEMP_DEBUG
	    && reload_gl_prog(&ws_temp_debug_prog, "ws_temp_debug_prog",
	    "ws_rain.vert", "ws_temp_debug.frag")
#endif
	    );
}

bool_t
librain_init(const char *the_pluginpath, const librain_glass_t *glass, size_t num)
{
	ASSERT(!inited);
	inited = B_TRUE;

	pluginpath = strdup(the_pluginpath);

	fdr_find(&drs.panel_render_type, "sim/graphics/view/panel_render_type");
	fdr_find(&drs.sim_time, "sim/time/total_running_time_sec");
	fdr_find(&drs.proj_matrix, "sim/graphics/view/projection_matrix");
	fdr_find(&drs.acf_matrix, "sim/graphics/view/acf_matrix");
	fdr_find(&drs.viewport, "sim/graphics/view/viewport");
	fdr_find(&drs.precip_rat,
	    "sim/weather/precipitation_on_aircraft_ratio");
	fdr_find(&drs.prop_thrust, "sim/flightmodel/engine/POINT_thrust");
	fdr_find(&drs.hdg, "sim/flightmodel/position/psi");
	fdr_find(&drs.beta, "sim/flightmodel/position/beta");
	fdr_find(&drs.gs, "sim/flightmodel/position/groundspeed");
	fdr_find(&drs.wind_dir, "sim/weather/wind_direction_degt");
	fdr_find(&drs.wind_spd, "sim/weather/wind_speed_kt");
	fdr_find(&drs.rot_rate, "sim/flightmodel/position/R");
	fdr_find(&drs.le_temp, "sim/weather/temperature_le_c");

	XPLMRegisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL);

	if (!GLEW_ARB_framebuffer_object) {
		logMsg("Cannot initialize: your OpenGL version doesn't appear "
		    "to support ARB_framebuffer_object");
		goto errout;
	}

	glGenTextures(1, &screenshot_tex);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 10, 10, 0, GL_RGB,
	    GL_UNSIGNED_BYTE, NULL);

	glGenFramebuffers(1, &screenshot_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, screenshot_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, screenshot_tex, 0);
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);

	water_effects_init(glass, num);

	if (!librain_reload_gl_progs())
		goto errout;

	return (B_TRUE);
errout:
	librain_fini();
	return (B_FALSE);
}

void
librain_fini(void)
{
	if (!inited)
		return;

	free(pluginpath);
	pluginpath = NULL;

	XPLMUnregisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL);

	DESTROY_OP(screenshot_fbo, 0, glDeleteFramebuffers(1, &screenshot_fbo));
	DESTROY_OP(screenshot_tex, 0, glDeleteTextures(1, &screenshot_tex));

	water_effects_fini();

	inited = B_FALSE;
}

GLuint
librain_get_screenshot_tex(void)
{
	return (screenshot_tex);
}