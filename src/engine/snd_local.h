// newen - what the sound files share
#pragma once
#include "common.h"
#include "mathlib.h"

// a loaded sound: 16-bit samples at the file's own rate, mono or stereo
typedef struct sfx_s {
	char   name[64];          // as asked for ("weapons/pl_gun3.wav")
	int    rate;
	int    channels;          // 1 or 2
	int    frames;            // samples per channel
	int    loopstart;         // -1 = plays once; else it loops back here until stopped
	short *data;              // frames * channels
	qboolean tried;           // loading was attempted (data NULL = missing)
} sfx_t;

// one word of a sentence ("hgrunt/squad!(e80)")
typedef struct {
	char  sample[64];         // "hgrunt/squad!.wav"
	short pitch, volume;      // 100 = as recorded
	short start, end;         // % of the sample to play
	short timecompress;       // % of each small block skipped (keeps the pitch)
} voxword_t;

typedef struct {
	char       name[32];      // "HG_ALERT0"
	voxword_t *words;
	int        numwords;
} sentence_t;

// snd_mem.c
sfx_t      *S_FindSfx(const char *name);       // cached; loads on first use (NULL data if missing)
void        S_FreeSfx(void);
int         S_SfxCount(void);
sfx_t      *S_SfxByIndex(int i);
qboolean    S_DecodeFile(const char *path, short **data, int *frames, int *rate, int *channels, int *loopstart);
void        S_LoadSentences(void);
sentence_t *S_FindSentence(const char *name);   // "HG_ALERT0" or "#12"
sentence_t *S_ParseSentence(const char *name, const char *text);   // for "speak"; caller frees with S_FreeSentence
void        S_FreeSentence(sentence_t *s);
