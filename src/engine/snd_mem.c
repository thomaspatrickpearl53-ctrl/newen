// newen - loading sounds: WAV (with loop points), and OGG, FLAC and MP3 for HD replacements; the
// sentence list (sound/sentences.txt)
//
// Half-Life's sounds are WAV files, mostly 8-bit mono at 11 kHz. A "cue" chunk marks a looping
// sound: it plays to the end, then jumps back to the cue until it's stopped. Everything is kept as
// 16-bit samples at the file's own rate; the mixer resamples. A .flac or .ogg next to a .wav (same
// name, any search path) is used instead of it.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "snd_local.h"
#include <ctype.h>

static sfx_t **sfx_list;
static int sfx_count;

// ---------------------------------------------------------------- WAV

static unsigned ReadU32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned ReadU16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static qboolean DecodeWav(const unsigned char *buf, size_t len, short **out, int *frames, int *rate, int *channels, int *loopstart)
{
	if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return false;
	int tag = 0, ch = 0, bits = 0, srate = 0;
	const unsigned char *data = NULL;
	size_t datalen = 0;
	*loopstart = -1;
	size_t pos = 12;
	while (pos + 8 <= len) {
		const unsigned char *c = buf + pos;
		size_t size = ReadU32(c + 4);
		if (size > len - pos - 8)
			size = len - pos - 8;   // some files lie about the last chunk's size
		if (!memcmp(c, "fmt ", 4) && size >= 16) {
			tag = (int)ReadU16(c + 8);
			ch = (int)ReadU16(c + 10);
			srate = (int)ReadU32(c + 12);
			bits = (int)ReadU16(c + 22);
			if (tag == 0xfffe && size >= 26)
				tag = (int)ReadU16(c + 32);   // WAVE_FORMAT_EXTENSIBLE: the real format's first two bytes
		} else if (!memcmp(c, "data", 4)) {
			data = c + 8;
			datalen = size;
		} else if (!memcmp(c, "cue ", 4) && size >= 28) {
			// the first cue point's sample offset is where the loop starts
			if (ReadU32(c + 8) > 0)
				*loopstart = (int)ReadU32(c + 8 + 4 + 20);
		}
		pos += 8 + size + (size & 1);
	}
	if (!data || ch < 1 || srate <= 0 || (tag != 1 && tag != 3))
		return false;
	int bytes = bits / 8;
	if (bytes < 1 || bytes > 4 || (tag == 3 && bytes != 4))
		return false;
	int n = (int)(datalen / ((size_t)bytes * ch));
	int outch = ch > 1 ? 2 : 1;
	short *s = Mem_Alloc(sizeof(short) * (size_t)(n > 0 ? n : 1) * outch);
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < outch; k++) {
			const unsigned char *p = data + ((size_t)i * ch + k) * bytes;
			int v;
			if (tag == 3) {
				float f;
				memcpy(&f, p, 4);
				v = (int)(f * 32767.0f);
			} else if (bytes == 1) {
				v = (p[0] - 128) << 8;
			} else {
				v = (signed char)p[bytes - 1] * 256 + p[bytes - 2];   // the top 16 bits
			}
			s[i * outch + k] = (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
		}
	}
	*out = s;
	*frames = n;
	*rate = srate;
	*channels = outch;
	if (*loopstart >= n)
		*loopstart = -1;
	return true;
}

// interleaved 16-bit from a decoder, any channel count, to our mono or stereo copy
static short *TakePcm(const short *pcm, int frames, int ch, int *outch)
{
	*outch = ch > 1 ? 2 : 1;
	short *s = Mem_Alloc(sizeof(short) * (size_t)(frames > 0 ? frames : 1) * *outch);
	for (int i = 0; i < frames; i++)
		for (int k = 0; k < *outch; k++)
			s[i * *outch + k] = pcm[(size_t)i * ch + k];
	return s;
}

qboolean S_DecodeFile(const char *path, short **data, int *frames, int *rate, int *channels, int *loopstart)
{
	size_t len = 0;
	unsigned char *buf = FS_LoadFile(path, &len);
	if (!buf)
		return false;
	qboolean ok = false;
	*loopstart = -1;
	const char *ext = strrchr(path, '.');
	if (ext && !stricmp(ext, ".ogg")) {
		int ch = 0, r = 0;
		short *pcm = NULL;
		int n = stb_vorbis_decode_memory(buf, (int)len, &ch, &r, &pcm);
		if (n > 0 && pcm) {
			*data = TakePcm(pcm, n, ch, channels);
			*frames = n;
			*rate = r;
			ok = true;
		}
		free(pcm);
	} else if (ext && !stricmp(ext, ".flac")) {
		unsigned ch = 0, r = 0;
		drflac_uint64 n = 0;
		short *pcm = drflac_open_memory_and_read_pcm_frames_s16(buf, len, &ch, &r, &n, NULL);
		if (pcm && n) {
			*data = TakePcm(pcm, (int)n, (int)ch, channels);
			*frames = (int)n;
			*rate = (int)r;
			ok = true;
		}
		drflac_free(pcm, NULL);
	} else if (ext && !stricmp(ext, ".mp3")) {
		drmp3_config cfg;
		drmp3_uint64 n = 0;
		short *pcm = drmp3_open_memory_and_read_pcm_frames_s16(buf, len, &cfg, &n, NULL);
		if (pcm && n) {
			*data = TakePcm(pcm, (int)n, (int)cfg.channels, channels);
			*frames = (int)n;
			*rate = (int)cfg.sampleRate;
			ok = true;
		}
		drmp3_free(pcm, NULL);
	} else {
		ok = DecodeWav(buf, len, data, frames, rate, channels, loopstart);
	}
	Mem_Free(buf);
	return ok;
}

// ---------------------------------------------------------------- the cache

static void LoadSfx(sfx_t *s)
{
	s->tried = true;
	char base[MAX_OSPATH];
	Q_strncpy(base, s->name, sizeof(base));
	char *ext = strrchr(base, '.');
	qboolean wav = ext && !stricmp(ext, ".wav");
	if (wav)
		*ext = 0;
	// an HD replacement next to the original
	static const char *hd[] = { ".flac", ".ogg" };
	for (int i = 0; wav && i < 2; i++) {
		char path[MAX_OSPATH];
		snprintf(path, sizeof(path), "sound/%s%s", base, hd[i]);
		if (FS_FileExists(path) &&
			S_DecodeFile(path, &s->data, &s->frames, &s->rate, &s->channels, &s->loopstart)) {
			// a loop point can only come from the original's cue chunk
			short *d = NULL;
			int f, r, c, loop = -1;
			if (S_DecodeFile(va("sound/%s", s->name), &d, &f, &r, &c, &loop) && loop >= 0 && r > 0)
				s->loopstart = (int)((double)loop * s->rate / r);
			Mem_Free(d);
			return;
		}
	}
	if (!S_DecodeFile(va("sound/%s", s->name), &s->data, &s->frames, &s->rate, &s->channels, &s->loopstart)) {
		s->data = NULL;
		Con_DPrintf("Sound %s not found\n", s->name);
	}
}

sfx_t *S_FindSfx(const char *name)
{
	if (!name || !*name)
		return NULL;
	// the name may be marked for the original engine (streamed, sentence group): skip the marks
	while (*name == '*' || *name == '#' || *name == '^' || *name == '@' || *name == ')' || *name == '<' || *name == '>')
		name++;
	for (int i = 0; i < sfx_count; i++)
		if (!stricmp(sfx_list[i]->name, name))
			return sfx_list[i]->data ? sfx_list[i] : NULL;
	sfx_t *s = Mem_Alloc(sizeof(sfx_t));
	Q_strncpy(s->name, name, sizeof(s->name));
	for (char *p = s->name; *p; p++)
		if (*p == '\\')
			*p = '/';
	sfx_list = Mem_Realloc(sfx_list, sizeof(*sfx_list) * (sfx_count + 1));
	sfx_list[sfx_count++] = s;
	LoadSfx(s);
	return s->data ? s : NULL;
}

void S_FreeSfx(void)
{
	for (int i = 0; i < sfx_count; i++) {
		Mem_Free(sfx_list[i]->data);
		Mem_Free(sfx_list[i]);
	}
	Mem_Free(sfx_list);
	sfx_list = NULL;
	sfx_count = 0;
}

int S_SfxCount(void) { return sfx_count; }
sfx_t *S_SfxByIndex(int i) { return i >= 0 && i < sfx_count ? sfx_list[i] : NULL; }

// ---------------------------------------------------------------- sentences

static sentence_t *sentences;
static int sentences_count;

typedef struct { short pitch, volume, start, end, timecompress; } voxopts_t;

// "(p110 t40)" -> options; returns the character after ')'
static const char *ParseOptions(const char *p, voxopts_t *o)
{
	p++;   // '('
	while (*p && *p != ')') {
		char c = (char)tolower((unsigned char)*p);
		if (strchr("pvset", c) && p[1] >= '0' && p[1] <= '9') {
			int v = atoi(p + 1);
			switch (c) {
			case 'p': o->pitch = (short)v; break;
			case 'v': o->volume = (short)v; break;
			case 's': o->start = (short)v; break;
			case 'e': o->end = (short)v; break;
			case 't': o->timecompress = (short)v; break;
			}
			p++;
			while (*p >= '0' && *p <= '9')
				p++;
		} else {
			p++;
		}
	}
	return *p == ')' ? p + 1 : p;
}

static void AddWord(sentence_t *s, const char *dir, const char *word, const voxopts_t *o)
{
	s->words = Mem_Realloc(s->words, sizeof(voxword_t) * (s->numwords + 1));
	voxword_t *w = &s->words[s->numwords++];
	snprintf(w->sample, sizeof(w->sample), "%s%s.wav", dir, word);
	w->pitch = o->pitch;
	w->volume = o->volume;
	w->start = o->start;
	w->end = o->end;
	w->timecompress = o->timecompress;
}

// "hgrunt/(t30) squad!, we!(e80) got!" -> words; a standalone (...) applies to the words after it, an
// attached one to its word only; a directory ("hgrunt/") applies to the words after it
static void ParseWords(sentence_t *s, const char *p)
{
	char dir[64] = "vox/";
	voxopts_t global = { 100, 100, 0, 100, 0 };
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '(') {
			p = ParseOptions(p, &global);
			continue;
		}
		char word[64];
		int n = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '(' && *p != ',' && !(*p == '.' && (p[1] == 0 || p[1] == ' '))) {
			if (n < (int)sizeof(word) - 1)
				word[n++] = *p;
			p++;
		}
		word[n] = 0;
		char *slash = strrchr(word, '/');
		if (slash) {
			size_t d = (size_t)(slash - word) + 1;
			if (d < sizeof(dir)) {
				memcpy(dir, word, d);
				dir[d] = 0;
			}
			memmove(word, slash + 1, strlen(slash + 1) + 1);
		}
		voxopts_t o = global;
		if (*p == '(') {
			if (word[0])
				p = ParseOptions(p, &o);
			else
				p = ParseOptions(p, &global);   // "hgrunt/(t30)": for the whole sentence
		}
		if (word[0])
			AddWord(s, dir, word, &o);
		if (*p == ',') {
			AddWord(s, dir, "_comma", &global);
			p++;
		} else if (*p == '.') {
			AddWord(s, dir, "_period", &global);
			p++;
		}
	}
}

sentence_t *S_ParseSentence(const char *name, const char *text)
{
	sentence_t *s = Mem_Alloc(sizeof(sentence_t));
	Q_strncpy(s->name, name, sizeof(s->name));
	ParseWords(s, text);
	return s;
}

void S_FreeSentence(sentence_t *s)
{
	if (!s)
		return;
	Mem_Free(s->words);
	Mem_Free(s);
}

void S_LoadSentences(void)
{
	for (int i = 0; i < sentences_count; i++)
		Mem_Free(sentences[i].words);
	Mem_Free(sentences);
	sentences = NULL;
	sentences_count = 0;

	char *data = FS_LoadFile("sound/sentences.txt", NULL);
	if (!data)
		return;
	char *line = data;
	while (*line) {
		char *end = line + strcspn(line, "\r\n");
		char save = *end;
		*end = 0;
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p && !(p[0] == '/' && p[1] == '/')) {
			char name[32];
			int n = 0;
			while (*p && *p != ' ' && *p != '\t') {
				if (n < (int)sizeof(name) - 1)
					name[n++] = *p;
				p++;
			}
			name[n] = 0;
			sentences = Mem_Realloc(sentences, sizeof(sentence_t) * (sentences_count + 1));
			sentence_t *s = &sentences[sentences_count++];
			memset(s, 0, sizeof(*s));
			Q_strncpy(s->name, name, sizeof(s->name));
			ParseWords(s, p);
		}
		*end = save;
		line = end;
		while (*line == '\r' || *line == '\n')
			line++;
	}
	Mem_Free(data);
	Con_DPrintf("sentences.txt: %d sentences\n", sentences_count);
}

sentence_t *S_FindSentence(const char *name)
{
	if (!name)
		return NULL;
	// by number: the game looks the name up in its own copy of sentences.txt and sends "!806"
	const char *num = name[0] == '#' ? name + 1 : name;
	if (*num >= '0' && *num <= '9') {
		int i = atoi(num);
		return i >= 0 && i < sentences_count ? &sentences[i] : NULL;
	}
	for (int i = 0; i < sentences_count; i++)
		if (!stricmp(sentences[i].name, name))
			return &sentences[i];
	return NULL;
}
