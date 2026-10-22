// newen - sound: channels, 3D placement, mixing, sentences, music and room echo
//
// Sounds play on channels. A sound belongs to an entity and one of its channels (weapon, voice,
// item, body...): a new sound on the same entity and channel replaces the old one, except on the
// "auto" and "static" channels. Each frame every channel's left/right volume is worked out from
// where its entity is, relative to the listener (distance falloff scaled by the attenuation, and
// constant-power stereo panning), and enough audio is mixed to keep about s_mixahead seconds queued
// on the SDL device. Samples are resampled on the fly (linear), which also gives pitch.
//
// Sentences ("!HG_ALERT0") play their words one after another on one channel, each with its own
// pitch, volume, start/end cut and time compression. Music (the cd and mp3 commands) is an MP3
// streamed from memory. room_type picks an echo preset, and being underwater muffles everything.
// A speaking entity's mouth opens with the loudness of its voice channel.
// Every sound that starts is handed to the closed captions (cl_caption.c) with its length and loudness.
#include "snd_local.h"
#include "server.h"
#include "dr_mp3.h"
#include <SDL.h>

#define CHAN_AUTO   0
#define CHAN_WEAPON 1
#define CHAN_VOICE  2
#define CHAN_STATIC 6

#define SND_CHANGE_VOL   (1<<0)
#define SND_CHANGE_PITCH (1<<1)
#define SND_STOP         (1<<5)

#define NOMINAL_CLIP_DIST 1000.0f

typedef struct {
	qboolean   active;
	sfx_t     *sfx;              // what's playing (a sentence's current word)
	int        entnum, entchannel;
	vec3_t     origin;
	qboolean   follow;           // the origin follows the entity
	float      vol, attn;
	int        pitch;
	double     pos, endpos;      // in the sample's frames
	double     wordstart;
	float      left, right;      // this frame's gains
	sentence_t *sentence;
	qboolean   own_sentence;     // made by "speak": freed with the channel
	int        word;
	float      wordvol;
	int        wordpitch, timecompress;
	double     mouth_sum;
	int        mouth_count, mouth;
	unsigned   started;          // for stealing the oldest
} channel_t;

static SDL_AudioDeviceID device;
static int out_rate;
static channel_t *chans;
static int chans_count;
static unsigned start_counter;
static cvar_t *volume, *mp3volume, *s_mixahead, *room_type, *waterroom_type, *s_maxchannels, *snd_rate, *s_show;
static vec3_t listener_origin, listener_right;
static qboolean listener_underwater;
static float *mixbuf;
static int mixbuf_frames;
static FILE *record_file;          // s_record
static int record_frames;

static struct {
	float  factor;               // 0..1 of the normal volume
	double start, fadeout, hold, fadein;
	float  target;
} fade = { 1 };

// ---------------------------------------------------------------- channels

static void FreeChannel(channel_t *ch)
{
	if (ch->own_sentence)
		S_FreeSentence(ch->sentence);
	memset(ch, 0, sizeof(*ch));
}

static channel_t *AllocChannel(void)
{
	for (int i = 0; i < chans_count; i++)
		if (!chans[i].active)
			return &chans[i];
	int max = (int)s_maxchannels->value;
	if (max < 16)
		max = 16;
	if (chans_count < max) {
		int n = chans_count + 32 > max ? max : chans_count + 32;
		chans = Mem_Realloc(chans, sizeof(channel_t) * n);
		memset(chans + chans_count, 0, sizeof(channel_t) * (n - chans_count));
		channel_t *ch = &chans[chans_count];
		chans_count = n;
		return ch;
	}
	// all busy: the quietest goes, the oldest if they're equal
	channel_t *best = &chans[0];
	for (int i = 1; i < chans_count; i++) {
		channel_t *c = &chans[i];
		float a = c->left + c->right, b = best->left + best->right;
		if (a < b || (a == b && c->started < best->started))
			best = c;
	}
	FreeChannel(best);
	return best;
}

static void Caption(channel_t *ch);

// the next word of a sentence that exists; false when the sentence is over
static qboolean StartWord(channel_t *ch, int i)
{
	for (; ch->sentence && i < ch->sentence->numwords; i++) {
		voxword_t *w = &ch->sentence->words[i];
		sfx_t *s = S_FindSfx(w->sample);
		if (!s)
			continue;
		ch->sfx = s;
		ch->word = i;
		ch->pos = ch->wordstart = (double)s->frames * w->start / 100.0;
		ch->endpos = (double)s->frames * (w->end > 0 ? w->end : 100) / 100.0;
		if (ch->endpos > s->frames)
			ch->endpos = s->frames;
		ch->wordpitch = w->pitch > 0 ? w->pitch : 100;
		ch->wordvol = w->volume / 100.0f;
		ch->timecompress = w->timecompress;
		return true;
	}
	return false;
}

static void EntityOrigin(int entnum, vec3_t out)
{
	if (!sv.active || entnum <= 0 || entnum >= sv.num_edicts)
		return;
	edict_t *e = EDICT_NUM(entnum);
	if (e->free)
		return;
	if (e->v.solid == SOLID_BSP || (e->v.absmin[0] || e->v.absmax[0]))
		for (int k = 0; k < 3; k++)
			out[k] = (e->v.absmin[k] + e->v.absmax[k]) * 0.5f;
	else
		VectorCopy(e->v.origin, out);
}

static qboolean SameSample(const channel_t *ch, const char *sample)
{
	if (!sample || !*sample)
		return true;
	if (sample[0] == '!')
		return ch->sentence && !stricmp(ch->sentence->name, sample + 1);
	while (*sample == '*' || *sample == '#' || *sample == '^' || *sample == '@' || *sample == ')')
		sample++;
	return !ch->sentence && ch->sfx && !stricmp(ch->sfx->name, sample);
}

void S_StopSound(int entnum, int entchannel, const char *sample)
{
	for (int i = 0; i < chans_count; i++) {
		channel_t *ch = &chans[i];
		if (ch->active && ch->entnum == entnum && ch->entchannel == entchannel && SameSample(ch, sample))
			FreeChannel(ch);
	}
}

void S_StopAllSounds(void)
{
	for (int i = 0; i < chans_count; i++)
		if (chans[i].active)
			FreeChannel(&chans[i]);
}

void S_StartSound(int entnum, int entchannel, const char *sample, const float *origin, float vol, float attn, int flags,
	int pitch)
{
	if (!device || !sample || !*sample)
		return;
	if (s_show->value > 1)
		Con_Printf("sound %s ent %d chan %d vol %.2f attn %.2f flags %d pitch %d\n", sample, entnum, entchannel, vol, attn,
			flags, pitch);
	if (flags & SND_STOP) {
		S_StopSound(entnum, entchannel, sample);
		return;
	}
	if (pitch <= 0)
		pitch = 100;
	if (flags & (SND_CHANGE_VOL | SND_CHANGE_PITCH)) {
		for (int i = 0; i < chans_count; i++) {
			channel_t *ch = &chans[i];
			if (ch->active && ch->entnum == entnum && ch->entchannel == entchannel && SameSample(ch, sample)) {
				if (flags & SND_CHANGE_VOL)
					ch->vol = vol;
				if (flags & SND_CHANGE_PITCH)
					ch->pitch = pitch;
				return;
			}
		}
		// nothing to change: GoldSrc starts it
	}
	sentence_t *sentence = NULL;
	sfx_t *sfx = NULL;
	if (sample[0] == '!') {
		sentence = S_FindSentence(sample + 1);
		if (!sentence) {
			Con_DPrintf("Sentence %s not found\n", sample + 1);
			return;
		}
	} else if (!(sfx = S_FindSfx(sample))) {
		return;
	}
	// a channel of an entity holds one sound at a time
	if (entchannel != CHAN_AUTO && entchannel != CHAN_STATIC)
		for (int i = 0; i < chans_count; i++)
			if (chans[i].active && chans[i].entnum == entnum && chans[i].entchannel == entchannel)
				FreeChannel(&chans[i]);

	channel_t *ch = AllocChannel();
	ch->active = true;
	ch->entnum = entnum;
	ch->entchannel = entchannel;
	ch->vol = vol;
	ch->attn = attn;
	ch->pitch = pitch;
	ch->started = ++start_counter;
	ch->follow = !origin && entnum > 0;
	if (origin)
		VectorCopy(origin, ch->origin);
	else
		EntityOrigin(entnum, ch->origin);
	ch->wordvol = 1;
	ch->wordpitch = 100;
	if (sentence) {
		ch->sentence = sentence;
		if (!StartWord(ch, 0))
			FreeChannel(ch);
		else
			Caption(ch);
		return;
	}
	ch->sfx = sfx;
	ch->pos = 0;
	ch->endpos = sfx->frames;
	Caption(ch);
}

void S_PreloadSound(const char *sample)
{
	if (device)
		S_FindSfx(sample);
}

// a sound with no place: the HUD, the "play" command
void S_LocalSound(const char *sample, float vol)
{
	S_StartSound(-1, CHAN_AUTO, sample, NULL, vol, 0, 0, 100);
}

// "speak": a sentence name, or words ("vox/hello world")
static void SpeakText(const char *text, float vol)
{
	if (!device || !text || !*text)
		return;
	if (S_FindSentence(text)) {
		S_StartSound(-1, CHAN_AUTO, va("!%s", text), NULL, vol, 0, 0, 100);
		return;
	}
	channel_t *ch = AllocChannel();
	ch->active = true;
	ch->entnum = -1;
	ch->vol = vol;
	ch->pitch = 100;
	ch->started = ++start_counter;
	ch->sentence = S_ParseSentence("speak", text);
	ch->own_sentence = true;
	if (!StartWord(ch, 0))
		FreeChannel(ch);
	else
		Caption(ch);
}

int S_MouthOpen(int entnum)
{
	int m = 0;
	for (int i = 0; i < chans_count; i++)
		if (chans[i].active && chans[i].entnum == entnum && chans[i].mouth > m)
			m = chans[i].mouth;
	return m;
}

void S_FadeVolume(float percent, float fadeout, float hold, float fadein)
{
	fade.start = host_realtime;
	fade.target = 1.0f - percent / 100.0f;
	fade.fadeout = fadeout;
	fade.hold = hold;
	fade.fadein = fadein;
}

static float FadeFactor(void)
{
	double t = host_realtime - fade.start;
	if (fade.start <= 0 || t < 0)
		return 1;
	if (t < fade.fadeout)
		return 1 - (1 - fade.target) * (float)(t / fade.fadeout);
	t -= fade.fadeout;
	if (t < fade.hold)
		return fade.target;
	t -= fade.hold;
	if (t < fade.fadein)
		return fade.target + (1 - fade.target) * (float)(t / fade.fadein);
	return 1;
}

// ---------------------------------------------------------------- placement

// false for sounds heard the same everywhere: no attenuation, no entity, or the local player's own
static qboolean IsPlaced(const channel_t *ch)
{
	edict_t *pl = SV_LocalPlayer();
	qboolean mine = pl && ch->entnum == NUM_FOR_EDICT(pl);
	return !(ch->attn <= 0 || ch->entnum < 0 || mine);
}

// distance falloff of a placed sound (1 close by, 0 out of earshot); dir gets the way to it
static float Falloff(const channel_t *ch, vec3_t dir)
{
	VectorSubtract(ch->origin, listener_origin, dir);
	float dist = VectorNormalize(dir) * (ch->attn / NOMINAL_CLIP_DIST);
	return dist >= 1 ? 0 : 1 - dist;
}

static void Spatialize(channel_t *ch, float master)
{
	float vol = ch->vol * ch->wordvol * master;
	if (!IsPlaced(ch)) {
		ch->left = ch->right = vol;
		return;
	}
	vec3_t d;
	float falloff = Falloff(ch, d);
	if (falloff <= 0) {
		ch->left = ch->right = 0;
		return;
	}
	// constant power: the same loudness wherever it is around you, 1.0 each side straight ahead
	float pan = DotProduct(listener_right, d);
	float a = (pan + 1) * (M_PI_F / 4);
	float scale = vol * falloff * 1.41421356f;
	ch->left = scale * cosf(a);
	ch->right = scale * sinf(a);
}

// seconds a sound plays at its pitch; a sentence's words with their cuts, pitch and time compression
static float PlayTime(const channel_t *ch)
{
	double t = 0;
	if (ch->sentence) {
		for (int i = 0; i < ch->sentence->numwords; i++) {
			const voxword_t *w = &ch->sentence->words[i];
			sfx_t *s = S_FindSfx(w->sample);
			if (!s || s->rate <= 0)
				continue;
			int end = w->end > 0 ? w->end : 100;
			double part = (double)s->frames * (end - w->start) / 100.0 / s->rate;
			part *= 100.0 / (w->pitch > 0 ? w->pitch : 100) * (1 - w->timecompress / 100.0);
			if (part > 0)
				t += part;
		}
	} else if (ch->sfx && ch->sfx->rate > 0) {
		t = (double)ch->sfx->frames / ch->sfx->rate;
	}
	return (float)(t * 100.0 / (ch->pitch > 0 ? ch->pitch : 100));
}

// how loud it is where the listener stands, without the volume setting (captions are for people
// who may have the sound turned down)
static void Caption(channel_t *ch)
{
	vec3_t d;
	float gain = ch->vol * (IsPlaced(ch) ? Falloff(ch, d) : 1);
	CC_SoundStarted(ch->sentence ? ch->sentence->name : ch->sfx->name, ch->sentence, PlayTime(ch), gain);
}

// ---------------------------------------------------------------- music

static struct {
	unsigned char *file;
	drmp3   mp3;
	qboolean open, loop, paused;
	char    name[MAX_OSPATH];
	float   buf[4096 * 2];       // decoded, stereo
	int     buffered;
	double  pos;                 // into buf
} music;

static void StopMusic(void)
{
	if (music.open)
		drmp3_uninit(&music.mp3);
	Mem_Free(music.file);
	music.file = NULL;
	music.open = false;
	music.paused = false;
	music.buffered = 0;
	music.pos = 0;
}

static void PlayMusic(const char *path, qboolean loop)
{
	StopMusic();
	size_t len = 0;
	music.file = FS_LoadFile(path, &len);
	if (!music.file) {
		Con_Printf("Music %s not found\n", path);
		return;
	}
	if (!drmp3_init_memory(&music.mp3, music.file, len, NULL)) {
		Con_Warnf("%s isn't an MP3 file\n", path);
		Mem_Free(music.file);
		music.file = NULL;
		return;
	}
	music.open = true;
	music.loop = loop;
	Q_strncpy(music.name, path, sizeof(music.name));
}

// refills the decode buffer, keeping the last frame for interpolation
static qboolean RefillMusic(void)
{
	if (music.buffered > 0) {
		music.buf[0] = music.buf[(music.buffered - 1) * 2];
		music.buf[1] = music.buf[(music.buffered - 1) * 2 + 1];
		music.pos -= music.buffered - 1;
		music.buffered = 1;
	}
	static float tmp[4096 * 2];
	int ch = music.mp3.channels > 0 ? (int)music.mp3.channels : 1;
	int want = 4096 - music.buffered;
	if (want > 4096 * 2 / ch)
		want = 4096 * 2 / ch;
	drmp3_uint64 got = drmp3_read_pcm_frames_f32(&music.mp3, want, tmp);
	if (!got && music.loop) {
		drmp3_seek_to_pcm_frame(&music.mp3, 0);
		got = drmp3_read_pcm_frames_f32(&music.mp3, want, tmp);
	}
	if (!got)
		return false;
	for (drmp3_uint64 i = 0; i < got; i++) {
		float l = tmp[i * ch], r = ch > 1 ? tmp[i * ch + 1] : l;
		music.buf[(music.buffered + i) * 2] = l;
		music.buf[(music.buffered + i) * 2 + 1] = r;
	}
	music.buffered += (int)got;
	return true;
}

static void MixMusic(float *out, int frames, float gain)
{
	if (!music.open || music.paused || gain <= 0)
		return;
	double step = (double)music.mp3.sampleRate / out_rate;
	for (int i = 0; i < frames; i++) {
		while ((int)music.pos + 1 >= music.buffered) {
			if (!RefillMusic()) {
				StopMusic();
				return;
			}
		}
		int ip = (int)music.pos;
		float f = (float)(music.pos - ip);
		for (int k = 0; k < 2; k++) {
			float a = music.buf[ip * 2 + k], b = music.buf[(ip + 1) * 2 + k];
			out[i * 2 + k] += (a + (b - a) * f) * 32767.0f * gain;
		}
		music.pos += step;
	}
}

// the original game's CD tracks are MP3s in media/ (track 2 is the first piece)
static const char *cd_tracks[] = {
	"", "", "Half-Life01", "Prospero01", "Half-Life12", "Half-Life07", "Half-Life10", "Suspense01", "Suspense03",
	"Half-Life09", "Half-Life02", "Half-Life13", "Half-Life04", "Half-Life15", "Half-Life14", "Half-Life16",
	"Suspense02", "Half-Life03", "Half-Life08", "Prospero02", "Half-Life05", "Prospero04", "Half-Life11",
	"Half-Life06", "Prospero03", "Half-Life17", "Prospero05", "Suspense05", "Suspense07",
};

static void CD_f(void)
{
	const char *c = Cmd_Argv(1);
	if (!stricmp(c, "play") || !stricmp(c, "loop")) {
		int t = atoi(Cmd_Argv(2));
		if (t < 2 || t >= (int)(sizeof(cd_tracks) / sizeof(cd_tracks[0]))) {
			Con_DPrintf("cd: no track %d\n", t);
			return;
		}
		PlayMusic(va("media/%s.mp3", cd_tracks[t]), !stricmp(c, "loop"));
	} else if (!stricmp(c, "stop") || !stricmp(c, "off")) {
		StopMusic();
	} else if (!stricmp(c, "pause")) {
		music.paused = true;
	} else if (!stricmp(c, "resume")) {
		music.paused = false;
	} else if (!stricmp(c, "info")) {
		Con_Printf("%s%s\n", music.open ? music.name : "no music", music.paused ? " (paused)" : "");
	} else if (stricmp(c, "on")) {
		Con_Printf("usage: cd play|loop <track> | stop | pause | resume | info\n");
	}
}

static void MP3_f(void)
{
	const char *c = Cmd_Argv(1);
	if ((!stricmp(c, "play") || !stricmp(c, "loop")) && Cmd_Argc() > 2)
		PlayMusic(Cmd_Argv(2), !stricmp(c, "loop"));
	else if (!stricmp(c, "stop"))
		StopMusic();
	else
		Con_Printf("usage: mp3 play|loop <file> | stop\n");
}

// ---------------------------------------------------------------- room echo

// per room_type: delay (ms), feedback, wet mix, how dark the echo is (0 = bright)
static const struct { float delay, feedback, mix, damp; } rooms[] = {
	{ 0, 0, 0, 0 },                                                      // 0 off
	{ 65, 0.25f, 0.20f, 0.3f },                                          // 1 generic
	{ 15, 0.45f, 0.30f, 0.1f }, { 25, 0.50f, 0.30f, 0.1f }, { 35, 0.55f, 0.30f, 0.1f },     // metal
	{ 60, 0.40f, 0.30f, 0.3f }, { 90, 0.50f, 0.30f, 0.3f }, { 120, 0.60f, 0.30f, 0.3f },    // tunnel
	{ 40, 0.40f, 0.25f, 0.3f }, { 60, 0.45f, 0.25f, 0.3f }, { 80, 0.50f, 0.25f, 0.3f },     // chamber
	{ 30, 0.35f, 0.30f, 0.0f }, { 45, 0.40f, 0.30f, 0.0f }, { 60, 0.45f, 0.30f, 0.0f },     // bright
	{ 30, 0.20f, 0.20f, 0.8f }, { 40, 0.25f, 0.20f, 0.8f }, { 50, 0.30f, 0.20f, 0.8f },     // water
	{ 35, 0.35f, 0.25f, 0.4f }, { 55, 0.40f, 0.25f, 0.4f }, { 75, 0.50f, 0.25f, 0.4f },     // concrete
	{ 120, 0.20f, 0.20f, 0.5f }, { 170, 0.25f, 0.20f, 0.5f }, { 220, 0.30f, 0.20f, 0.5f },  // outside
	{ 80, 0.50f, 0.35f, 0.5f }, { 110, 0.55f, 0.35f, 0.5f }, { 140, 0.65f, 0.35f, 0.5f },   // cave
	{ 10, 0.70f, 0.40f, 0.2f }, { 20, 0.70f, 0.40f, 0.2f }, { 30, 0.70f, 0.40f, 0.2f },     // weird
};

static float *echo[2];
static int echo_size, echo_pos;
static float echo_lp[2], under_lp[2];

static void ApplyRoom(float *buf, int frames)
{
	int room = listener_underwater ? (int)waterroom_type->value : (int)room_type->value;
	int nrooms = (int)(sizeof(rooms) / sizeof(rooms[0]));
	if (room < 0 || room >= nrooms)
		room = 0;
	if (!echo_size) {
		echo_size = out_rate;   // a second of history
		echo[0] = Mem_Alloc(sizeof(float) * echo_size);
		echo[1] = Mem_Alloc(sizeof(float) * echo_size);
	}
	float fb = rooms[room].feedback, mix = rooms[room].mix, damp = rooms[room].damp;
	// slightly different delays per side make it wider
	int dl = (int)(rooms[room].delay * out_rate / 1000.0f), dr = (int)(dl * 1.13f);
	for (int i = 0; i < frames; i++) {
		for (int k = 0; k < 2; k++) {
			float in = buf[i * 2 + k];
			float out = in;
			if (room) {
				int d = k ? dr : dl;
				float delayed = echo[k][(echo_pos - d + echo_size) % echo_size];
				out = in + delayed * mix;
				echo_lp[k] += (in + delayed * fb - echo_lp[k]) * (1 - damp);
				echo[k][echo_pos] = echo_lp[k];
			} else {
				echo[k][echo_pos] = 0;
			}
			if (listener_underwater) {
				// muffled
				under_lp[k] += (out - under_lp[k]) * 0.12f;
				out = under_lp[k];
			}
			buf[i * 2 + k] = out;
		}
		echo_pos = (echo_pos + 1) % echo_size;
	}
}

// ---------------------------------------------------------------- mixing

static void MixChannel(channel_t *ch, float *out, int frames)
{
	qboolean voice = ch->entnum > 0 && (ch->entchannel == CHAN_VOICE || ch->sentence);
	for (int i = 0; i < frames; i++) {
		sfx_t *s = ch->sfx;
		if (ch->pos >= ch->endpos) {
			if (ch->sentence) {
				if (!StartWord(ch, ch->word + 1)) {
					FreeChannel(ch);
					return;
				}
				s = ch->sfx;
			} else if (s->loopstart >= 0) {
				ch->pos = s->loopstart + (ch->pos - ch->endpos);
				if (ch->pos >= ch->endpos)
					ch->pos = s->loopstart;
			} else {
				FreeChannel(ch);
				return;
			}
		}
		if (ch->timecompress > 0) {
			// keeps the pitch and skips the end of every 25 ms
			double block = s->rate / 40.0, off = fmod(ch->pos - ch->wordstart, block);
			if (off >= block * (100 - ch->timecompress) / 100.0)
				ch->pos += block - off;
			if (ch->pos >= ch->endpos)
				continue;
		}
		int ip = (int)ch->pos;
		float f = (float)(ch->pos - ip);
		int next = ip + 1 < s->frames ? ip + 1 : (s->loopstart >= 0 ? s->loopstart : ip);
		float l, r;
		if (s->channels == 2) {
			l = s->data[ip * 2] + (s->data[next * 2] - s->data[ip * 2]) * f;
			r = s->data[ip * 2 + 1] + (s->data[next * 2 + 1] - s->data[ip * 2 + 1]) * f;
		} else {
			l = r = s->data[ip] + (s->data[next] - s->data[ip]) * f;
		}
		out[i * 2] += l * ch->left;
		out[i * 2 + 1] += r * ch->right;
		if (voice) {
			ch->mouth_sum += fabsf(l);
			ch->mouth_count++;
		}
		ch->pos += (double)s->rate * ch->pitch * ch->wordpitch / (100.0 * 100.0 * out_rate);
	}
}

static void Mix(int frames)
{
	if (frames > mixbuf_frames) {
		mixbuf_frames = frames;
		mixbuf = Mem_Realloc(mixbuf, sizeof(float) * 2 * frames);
	}
	memset(mixbuf, 0, sizeof(float) * 2 * frames);
	for (int i = 0; i < chans_count; i++)
		if (chans[i].active)
			MixChannel(&chans[i], mixbuf, frames);
	ApplyRoom(mixbuf, frames);
	MixMusic(mixbuf, frames, mp3volume->value * volume->value);

	static short *pcm;
	static int pcm_frames;
	if (frames > pcm_frames) {
		pcm_frames = frames;
		pcm = Mem_Realloc(pcm, sizeof(short) * 2 * frames);
	}
	for (int i = 0; i < frames * 2; i++) {
		float v = mixbuf[i];
		pcm[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	SDL_QueueAudio(device, pcm, (Uint32)(sizeof(short) * 2 * frames));
	if (record_file) {
		fwrite(pcm, sizeof(short) * 2, (size_t)frames, record_file);
		record_frames += frames;
	}
}

// ---------------------------------------------------------------- recording what's heard

static void WriteWavHeader(FILE *f, int frames)
{
	unsigned data = (unsigned)frames * 4, v;
	fwrite("RIFF", 1, 4, f);
	v = 36 + data; fwrite(&v, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	v = 16; fwrite(&v, 4, 1, f);
	unsigned short s = 1; fwrite(&s, 2, 1, f);        // PCM
	s = 2; fwrite(&s, 2, 1, f);                       // stereo
	v = (unsigned)out_rate; fwrite(&v, 4, 1, f);
	v = (unsigned)out_rate * 4; fwrite(&v, 4, 1, f);
	s = 4; fwrite(&s, 2, 1, f);
	s = 16; fwrite(&s, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
}

static void StopRecording(void)
{
	if (!record_file)
		return;
	fseek(record_file, 0, SEEK_SET);
	WriteWavHeader(record_file, record_frames);
	fclose(record_file);
	record_file = NULL;
	Con_Printf("Recorded %.1f seconds of sound\n", (double)record_frames / out_rate);
}

static void Record_f(void)
{
	if (record_file) {
		StopRecording();
		return;
	}
	if (!device) {
		Con_Printf("Sound is off\n");
		return;
	}
	const char *name = Cmd_Argc() > 1 ? Cmd_Argv(1) : "sound.wav";
	record_file = FS_OpenWrite(name);
	if (!record_file) {
		Con_Printf("Couldn't write %s\n", name);
		return;
	}
	record_frames = 0;
	WriteWavHeader(record_file, 0);
	Con_Printf("Recording the sound to %s; s_record again stops\n", name);
}

void S_Update(const vec3_t origin, const vec3_t right)
{
	if (!device)
		return;
	VectorCopy(origin, listener_origin);
	VectorCopy(right, listener_right);
	listener_underwater = world.loaded && BSP_PointInLeaf(origin)->contents <= CONTENTS_WATER &&
		BSP_PointInLeaf(origin)->contents >= CONTENTS_LAVA;

	float master = volume->value * FadeFactor();
	int playing = 0;
	for (int i = 0; i < chans_count; i++) {
		channel_t *ch = &chans[i];
		if (!ch->active)
			continue;
		if (ch->follow)
			EntityOrigin(ch->entnum, ch->origin);
		Spatialize(ch, master);
		// the mouth follows the voice's average loudness since the last frame (0..64)
		if (ch->mouth_count) {
			ch->mouth = (int)(ch->mouth_sum / ch->mouth_count / 100.0);
			if (ch->mouth > 64)
				ch->mouth = 64;
			ch->mouth_sum = 0;
			ch->mouth_count = 0;
		}
		playing++;
	}
	if (s_show->value)
		Con_Printf("%d channels playing\n", playing);

	int queued = (int)(SDL_GetQueuedAudioSize(device) / (sizeof(short) * 2));
	int target = (int)(s_mixahead->value * out_rate);
	if (target < out_rate / 50)
		target = out_rate / 50;
	if (target > out_rate)
		target = out_rate;
	if (queued < target)
		Mix(target - queued);
}

// ---------------------------------------------------------------- commands

static void Play_f(void)
{
	for (int i = 1; i < Cmd_Argc(); i++) {
		const char *name = Cmd_Argv(i);
		S_LocalSound(strchr(name, '.') ? name : va("%s.wav", name), 1);
	}
}

static void PlayVol_f(void)
{
	for (int i = 1; i + 1 < Cmd_Argc(); i += 2) {
		const char *name = Cmd_Argv(i);
		S_LocalSound(strchr(name, '.') ? name : va("%s.wav", name), (float)atof(Cmd_Argv(i + 1)));
	}
}

static void Speak_f(void)
{
	if (Cmd_Argc() < 2) {
		Con_Printf("usage: speak <sentence name or words, e.g. \"vox/hello world\">\n");
		return;
	}
	char text[1024] = "";
	for (int i = 1; i < Cmd_Argc(); i++) {
		if (i > 1)
			strncat(text, " ", sizeof(text) - strlen(text) - 1);
		strncat(text, Cmd_Argv(i), sizeof(text) - strlen(text) - 1);
	}
	SpeakText(text, 1);
}

static void StopSound_f(void)
{
	S_StopAllSounds();
	StopMusic();
}

static void SoundList_f(void)
{
	int bytes = 0;
	for (int i = 0; i < S_SfxCount(); i++) {
		sfx_t *s = S_SfxByIndex(i);
		if (!s->data)
			continue;
		bytes += s->frames * s->channels * 2;
		Con_Printf("%6d Hz %s %s%s\n", s->rate, s->channels == 2 ? "stereo" : "mono  ", s->name,
			s->loopstart >= 0 ? " (loops)" : "");
	}
	Con_Printf("%d sounds, %d KB\n", S_SfxCount(), bytes / 1024);
}

static void SoundInfo_f(void)
{
	if (!device) {
		Con_Printf("Sound is off\n");
		return;
	}
	Con_Printf("%d Hz stereo, %d channel slots, room %d%s\n", out_rate, chans_count,
		listener_underwater ? (int)waterroom_type->value : (int)room_type->value, listener_underwater ? " (underwater)" : "");
}

// ---------------------------------------------------------------- startup

void S_Init(void)
{
	volume = Cvar_Get("volume", "0.8", FCVAR_ARCHIVE, "sound volume (0..1)");
	mp3volume = Cvar_Get("MP3Volume", "0.8", FCVAR_ARCHIVE, "music volume (0..1, times volume)");
	s_mixahead = Cvar_Get("s_mixahead", "0.08", FCVAR_ARCHIVE, "seconds of sound mixed ahead (more = fewer gaps, more delay)");
	room_type = Cvar_Get("room_type", "0", 0, "room echo preset (0 off, 1-28 as in GoldSrc); maps set it");
	waterroom_type = Cvar_Get("waterroom_type", "14", 0, "room echo preset under water");
	s_maxchannels = Cvar_Get("s_maxchannels", "128", FCVAR_ARCHIVE, "most sounds playing at once");
	snd_rate = Cvar_Get("snd_rate", "48000", FCVAR_ARCHIVE, "output sample rate (44100 or 48000; applies at start)");
	s_show = Cvar_Get("s_show", "0", 0, "1 prints how many sounds are playing, 2 also every sound that starts");
	Cmd_AddCommand("play", Play_f, "play <sound> - plays a sound file (sound/ is implied)");
	Cmd_AddCommand("playvol", PlayVol_f, "playvol <sound> <volume>");
	Cmd_AddCommand("speak", Speak_f, "speak <sentence or words> - plays a sentence");
	Cmd_AddCommand("spk", Speak_f, "same as speak (the game uses it)");
	Cmd_AddCommand("stopsound", StopSound_f, "stops every sound and the music");
	Cmd_AddCommand("soundlist", SoundList_f, "lists the loaded sounds");
	Cmd_AddCommand("soundinfo", SoundInfo_f, "shows the sound device");
	Cmd_AddCommand("cd", CD_f, "cd play|loop <track> | stop | pause | resume - the game's music");
	Cmd_AddCommand("mp3", MP3_f, "mp3 play|loop <file> | stop");
	Cmd_AddCommand("s_record", Record_f, "s_record [file.wav] - records what you hear into the game's userdata folder; again stops");

	S_LoadSentences();
	if (COM_CheckParm("-nosound"))
		return;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		Con_Warnf("No sound: %s\n", SDL_GetError());
		return;
	}
	SDL_AudioSpec want, have;
	memset(&want, 0, sizeof(want));
	want.freq = snd_rate->value >= 8000 ? (int)snd_rate->value : 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!device) {
		Con_Warnf("No sound: %s\n", SDL_GetError());
		return;
	}
	out_rate = have.freq;
	SDL_PauseAudioDevice(device, 0);
	Con_Printf("Sound: %d Hz stereo (%s)\n", out_rate, SDL_GetCurrentAudioDriver());
}

void S_Shutdown(void)
{
	StopRecording();
	StopMusic();
	S_StopAllSounds();
	if (device)
		SDL_CloseAudioDevice(device);
	device = 0;
}
