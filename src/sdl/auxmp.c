#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL/SDL.h>
#include <libxmp-lite/xmp.h>
#include "audio.h"
#include "cfgopt.h"
#include "assfile/assfile.h"

#define MIX_FREQ	22050

static xmp_context ctx;
static SDL_AudioSpec spec;
static struct au_module *curmod;
static volatile int mod_ended;
static int vol_master, vol_mus, vol_sfx;

static void audio_callback(void *cls, Uint8 *stream, int len);
static void set_mus_vol(void);

int au_init(void)
{
	SDL_AudioSpec desired = {0};

	curmod = 0;
	vol_master = vol_mus = vol_sfx = 255;

	if(!opt.music) return 0;

	if(!(ctx = xmp_create_context())) {
		fprintf(stderr, "au_init: failed to create xmp context\n");
		return -1;
	}

	desired.freq = MIX_FREQ;
	desired.format = AUDIO_S16MSB;
	desired.channels = 2;
	desired.samples = 2048;
	desired.callback = audio_callback;

	if(SDL_OpenAudio(&desired, &spec) < 0) {
		fprintf(stderr, "au_init: failed to open audio: %s\n", SDL_GetError());
		xmp_free_context(ctx);
		ctx = 0;
		return -1;
	}
	printf("audio: %d Hz, %d channels, format %04x, %d samples\n", spec.freq,
			spec.channels, spec.format, spec.samples);
	return 0;
}

void au_shutdown(void)
{
	if(!ctx) return;

	au_stop_module(curmod);
	SDL_CloseAudio();
	xmp_free_context(ctx);
	ctx = 0;
}

struct au_module *au_load_module(const char *fname)
{
	struct au_module *mod;
	struct xmp_module_info mi;
	ass_file *fp;
	char *buf;
	long size;
	const char *name;

	if(!ctx) return 0;

	if(!(fp = ass_fopen(fname, "rb"))) {
		fprintf(stderr, "au_load_module: failed to open: %s\n", fname);
		return 0;
	}
	size = ass_fseek(fp, 0, SEEK_END);
	ass_fseek(fp, 0, SEEK_SET);

	if(size <= 0 || !(buf = malloc(size))) {
		fprintf(stderr, "au_load_module: failed to allocate %ld bytes\n", size);
		ass_fclose(fp);
		return 0;
	}
	if(ass_fread(buf, 1, size, fp) != (size_t)size) {
		fprintf(stderr, "au_load_module: failed to read: %s\n", fname);
		free(buf);
		ass_fclose(fp);
		return 0;
	}
	ass_fclose(fp);

	if(xmp_load_module_from_memory(ctx, buf, size) != 0) {
		fprintf(stderr, "au_load_module: failed to load module: %s\n", fname);
		free(buf);
		return 0;
	}
	free(buf);

	if(!(mod = malloc(sizeof *mod))) {
		xmp_release_module(ctx);
		return 0;
	}

	xmp_get_module_info(ctx, &mi);
	name = mi.mod->name[0] ? mi.mod->name : fname;
	if(!(mod->name = malloc(strlen(name) + 1))) {
		xmp_release_module(ctx);
		free(mod);
		return 0;
	}
	strcpy(mod->name, name);
	mod->impl = ctx;

	printf("loaded module \"%s\" (%s)\n", mod->name, fname);
	return mod;
}

void au_free_module(struct au_module *mod)
{
	if(!mod) return;

	if(mod == curmod) {
		au_stop_module(curmod);
	}
	xmp_release_module(ctx);
	free(mod->name);
	free(mod);
}

int au_play_module(struct au_module *mod)
{
	int fmt = 0;

	if(!mod) return -1;
	if(curmod) {
		if(curmod == mod) return 0;
		au_stop_module(curmod);
	}

	if(spec.format == AUDIO_S8 || spec.format == AUDIO_U8) {
		fmt |= XMP_FORMAT_8BIT;
	}
	if(spec.format == AUDIO_U8 || spec.format == AUDIO_U16LSB || spec.format == AUDIO_U16MSB) {
		fmt |= XMP_FORMAT_UNSIGNED;
	}
	if(spec.format == AUDIO_S16LSB || spec.format == AUDIO_U16LSB) {
		fmt |= XMP_FORMAT_BYTESWAP;
	}
	if(spec.channels == 1) {
		fmt |= XMP_FORMAT_MONO;
	}

	/* mix at the rate the hardware gave us, to avoid resampling */
	if(xmp_start_player(ctx, spec.freq, fmt) != 0) {
		fprintf(stderr, "au_play_module: failed to start player\n");
		return -1;
	}
	mod_ended = 0;
	curmod = mod;
	set_mus_vol();
	SDL_PauseAudio(0);
	return 0;
}

void au_update(void)
{
	if(curmod && mod_ended) {
		au_stop_module(curmod);
	}
}

int au_stop_module(struct au_module *mod)
{
	if(mod && curmod != mod) return -1;
	if(!curmod) return -1;

	SDL_PauseAudio(1);
	SDL_LockAudio();
	curmod = 0;
	xmp_end_player(ctx);
	SDL_UnlockAudio();
	return 0;
}

int au_module_state(struct au_module *mod)
{
	if(mod) {
		return curmod == mod ? AU_PLAYING : AU_STOPPED;
	}
	return curmod ? AU_PLAYING : AU_STOPPED;
}

int au_player_pos(void)
{
	struct xmp_frame_info fi;

	if(!curmod) return -1;

	xmp_get_frame_info(ctx, &fi);
	return fi.pos;
}

int au_volume(int vol)
{
	AU_VOLADJ(vol_master, vol);
	if(vol != vol_master) {
		vol_master = vol;
		set_mus_vol();
	}
	return vol_master;
}

int au_sfx_volume(int vol)
{
	AU_VOLADJ(vol_sfx, vol);
	vol_sfx = vol;
	return vol_sfx;
}

int au_music_volume(int vol)
{
	AU_VOLADJ(vol_mus, vol);
	vol_mus = vol;
	set_mus_vol();
	return vol_mus;
}

static void set_mus_vol(void)
{
	if(curmod) {
		xmp_set_player(ctx, XMP_PLAYER_VOLUME, vol_mus * vol_master * 100 / (255 * 255));
	}
}

static void audio_callback(void *cls, Uint8 *stream, int len)
{
	if(!curmod || mod_ended || xmp_play_buffer(ctx, stream, len, 1) != 0) {
		mod_ended = curmod != 0;
		memset(stream, spec.silence, len);
	}
}
