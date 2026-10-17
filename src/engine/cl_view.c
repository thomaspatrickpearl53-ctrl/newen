// newen - view effects the engine owns in GoldSrc: titles.txt text messages, screen fades and
// screen shakes
//
// titles.txt holds named messages (the opening credits, game_text) with their position, colours
// and timing; the client's HUD draws them and asks for them by name. TE_TEXTMESSAGE builds one on
// the fly in a "__NETMESSAGE__" slot and hands the name to the client's HudText hook. ScreenFade
// and ScreenShake are user messages the engine handles when the client doesn't hook them.
#include "cl_local.h"

static client_textmessage_t *titles;
static int titles_count;

#define NETMESSAGES 4
static client_textmessage_t netmsg[NETMESSAGES];
static char netmsg_name[NETMESSAGES][32];
static char netmsg_text[NETMESSAGES][512];
static int netmsg_next;

static screenfade_t fade;

static struct {
	double time;        // when it ends
	float  duration, amplitude, frequency;
	double next;        // next new random offset
	vec3_t offset, applied_offset;
	float  angle, applied_angle;
} shake;

// ---------------------------------------------------------------- titles.txt

static const char *SkipSpace(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

// one line into buf (without the end of line); returns the next line or NULL at the end
static const char *ReadLine(const char *p, char *buf, int size)
{
	if (!*p)
		return NULL;
	int n = 0;
	while (*p && *p != '\n' && *p != '\r') {
		if (n < size - 1)
			buf[n++] = *p;
		p++;
	}
	buf[n] = 0;
	if (*p == '\r')
		p++;
	if (*p == '\n')
		p++;
	return p;
}

void CL_LoadTitles(void)
{
	char *data = FS_LoadFile("titles.txt", NULL);
	if (!data)
		return;
	client_textmessage_t cur;
	memset(&cur, 0, sizeof(cur));
	cur.x = cur.y = -1;
	cur.r1 = cur.g1 = cur.b1 = cur.a1 = 255;
	cur.r2 = cur.g2 = cur.b2 = cur.a2 = 255;
	cur.holdtime = 2;

	char line[1024], name[64] = "";
	const char *p = data;
	while ((p = ReadLine(p, line, sizeof(line))) != NULL) {
		const char *s = SkipSpace(line);
		if (!*s || (s[0] == '/' && s[1] == '/'))
			continue;
		if (*s == '$') {
			float a = 0, b = 0, c = 0;
			int n;
			if (!strnicmp(s, "$position", 9) && sscanf(s + 9, "%f %f", &a, &b) == 2) {
				cur.x = a;
				cur.y = b;
			} else if (!strnicmp(s, "$effect", 7) && sscanf(s + 7, "%d", &n) == 1) {
				cur.effect = n;
			} else if (!strnicmp(s, "$color2", 7) && sscanf(s + 7, "%f %f %f", &a, &b, &c) == 3) {
				cur.r2 = (unsigned char)a; cur.g2 = (unsigned char)b; cur.b2 = (unsigned char)c;
			} else if (!strnicmp(s, "$color", 6) && sscanf(s + 6, "%f %f %f", &a, &b, &c) == 3) {
				cur.r1 = (unsigned char)a; cur.g1 = (unsigned char)b; cur.b1 = (unsigned char)c;
			} else if (!strnicmp(s, "$fadein", 7)) {
				cur.fadein = (float)atof(s + 7);
			} else if (!strnicmp(s, "$fadeout", 8)) {
				cur.fadeout = (float)atof(s + 8);
			} else if (!strnicmp(s, "$holdtime", 9)) {
				cur.holdtime = (float)atof(s + 9);
			} else if (!strnicmp(s, "$fxtime", 7)) {
				cur.fxtime = (float)atof(s + 7);
			}
			continue;
		}
		if (*s == '{') {
			// the message text runs to the closing brace, line breaks and all
			strbuf_t text = { 0 };
			qboolean first = true;
			while ((p = ReadLine(p, line, sizeof(line))) != NULL) {
				if (SkipSpace(line)[0] == '}')
					break;
				if (!first)
					SB_Append(&text, "\n", 1);
				SB_Append(&text, line, strlen(line));
				first = false;
			}
			titles = Mem_Realloc(titles, sizeof(client_textmessage_t) * (titles_count + 1));
			client_textmessage_t *m = &titles[titles_count++];
			*m = cur;
			m->pName = Mem_Strdup(name);
			m->pMessage = Mem_Strdup(text.data ? text.data : "");
			SB_Free(&text);
			if (!p)
				break;
			continue;
		}
		// a name line: the next "{" starts its text
		Q_strncpy(name, s, sizeof(name));
		size_t n = strlen(name);
		while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
			name[--n] = 0;
	}
	Mem_Free(data);
	Con_DPrintf("titles.txt: %d messages\n", titles_count);
}

client_textmessage_t *CL_TextMessageGet(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < NETMESSAGES; i++)
		if (netmsg[i].pName && !stricmp(netmsg_name[i], name))
			return &netmsg[i];
	for (int i = 0; i < titles_count; i++)
		if (!stricmp(titles[i].pName, name))
			return &titles[i];
	return NULL;
}

void CL_ShowNetMessage(const client_textmessage_t *msg)
{
	int slot = netmsg_next++ % NETMESSAGES;
	snprintf(netmsg_name[slot], sizeof(netmsg_name[slot]), "__NETMESSAGE__%d", slot + 1);
	Q_strncpy(netmsg_text[slot], msg->pMessage ? msg->pMessage : "", sizeof(netmsg_text[slot]));
	netmsg[slot] = *msg;
	netmsg[slot].pName = netmsg_name[slot];
	netmsg[slot].pMessage = netmsg_text[slot];
	// the client shows it like any other title, by name
	pfnUserMsgHook fn = CL_FindMessageHook("HudText");
	if (fn) {
		char buf[64];
		int len = (int)strlen(netmsg_name[slot]) + 1;
		memcpy(buf, netmsg_name[slot], len);
		CLIENT_CALL("HudText", fn("HudText", len, buf));
	}
}

// ---------------------------------------------------------------- fade and shake

#define FFADE_OUT      0x0001
#define FFADE_MODULATE 0x0002
#define FFADE_STAYOUT  0x0004

static int ReadU16(const unsigned char *d) { return d[0] | (d[1] << 8); }

qboolean CL_EngineMessage(const char *name, const unsigned char *data, int len)
{
	if (!stricmp(name, "ScreenFade") && len >= 10) {
		float duration = ReadU16(data) / 4096.0f, hold = ReadU16(data + 2) / 4096.0f;
		int flags = (short)ReadU16(data + 4);
		memset(&fade, 0, sizeof(fade));
		fade.fader = data[6];
		fade.fadeg = data[7];
		fade.fadeb = data[8];
		fade.fadealpha = data[9];
		fade.fadeFlags = flags;
		fade.fadeEnd = duration;
		fade.fadeReset = hold;
		float t = (float)cl_time;
		if (flags & FFADE_OUT) {
			// to the colour over the duration, then hold
			if (fade.fadeEnd)
				fade.fadeSpeed = -(float)fade.fadealpha / fade.fadeEnd;
			fade.fadeEnd += t;
			fade.fadeReset += fade.fadeEnd;
		} else {
			// hold, then back from the colour over the duration
			if (fade.fadeEnd)
				fade.fadeSpeed = (float)fade.fadealpha / fade.fadeEnd;
			fade.fadeReset += t;
			fade.fadeEnd += fade.fadeReset;
		}
		fade.fadeTotalEnd = fade.fadeReset;
		return true;
	}
	if (!stricmp(name, "ScreenShake") && len >= 6) {
		float amplitude = ReadU16(data) / 4096.0f, duration = ReadU16(data + 2) / 4096.0f;
		float frequency = ReadU16(data + 4) / 256.0f;
		// a weaker shake doesn't cut a stronger one short
		if (cl_time > shake.time || amplitude > shake.amplitude) {
			shake.amplitude = amplitude;
			shake.duration = duration;
			shake.time = cl_time + duration;
			shake.frequency = frequency;
			shake.next = 0;
		}
		return true;
	}
	return false;
}

void CL_GetScreenFade(screenfade_t *f) { *f = fade; }
void CL_SetScreenFade(const screenfade_t *f) { fade = *f; }

void CL_DrawScreenFade(void)
{
	float t = (float)cl_time;
	int alpha;
	if (t > fade.fadeReset && t > fade.fadeEnd && !(fade.fadeFlags & FFADE_STAYOUT))
		return;
	if (fade.fadeFlags & FFADE_STAYOUT) {
		alpha = fade.fadealpha;
	} else {
		float a = fade.fadeSpeed * (fade.fadeEnd - t);
		if (fade.fadeFlags & FFADE_OUT)
			a += fade.fadealpha;
		alpha = (int)a;
		if (alpha < 0) alpha = 0;
		if (alpha > fade.fadealpha) alpha = fade.fadealpha;
	}
	if (alpha <= 0)
		return;
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	if (fade.fadeFlags & FFADE_MODULATE) {
		// darkens towards the colour instead of covering
		float k = alpha / 255.0f;
		glBlendFunc(GL_ZERO, GL_SRC_COLOR);
		glColor4f(1 - k + k * fade.fader / 255.0f, 1 - k + k * fade.fadeg / 255.0f, 1 - k + k * fade.fadeb / 255.0f, 1);
	} else {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColor4ub(fade.fader, fade.fadeg, fade.fadeb, (GLubyte)alpha);
	}
	glBegin(GL_QUADS);
	glVertex2i(0, 0);
	glVertex2i(vid_width, 0);
	glVertex2i(vid_width, vid_height);
	glVertex2i(0, vid_height);
	glEnd();
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1, 1, 1, 1);
}

// called by the client's V_CalcRefdef once a frame
void CL_CalcShake(void)
{
	if (cl_time > shake.time || shake.amplitude <= 0 || shake.frequency <= 0 || shake.duration <= 0) {
		VectorClear(shake.applied_offset);
		shake.applied_angle = 0;
		return;
	}
	if (cl_time > shake.next) {
		shake.next = cl_time + 1.0 / shake.frequency;
		for (int i = 0; i < 3; i++)
			shake.offset[i] = CL_Random(-shake.amplitude, shake.amplitude);
		shake.angle = CL_Random(-shake.amplitude * 0.25f, shake.amplitude * 0.25f);
	}
	// fades out over the duration, wobbling faster as it does
	float fraction = (float)((shake.time - cl_time) / shake.duration);
	float freq = fraction > 0 ? shake.frequency / fraction : 0;
	fraction *= fraction;
	float angle = (float)cl_time * freq;
	if (angle > 1e8f)
		angle = 1e8f;
	fraction *= sinf(angle);
	VectorScale(shake.offset, fraction, shake.applied_offset);
	shake.applied_angle = shake.angle * fraction;
	shake.amplitude -= shake.amplitude * (float)(host_frametime / (shake.duration * shake.frequency));
}

void CL_ApplyShake(float *origin, float *angles, float factor)
{
	if (origin)
		VectorMA(origin, factor, shake.applied_offset, origin);
	if (angles)
		angles[2] += shake.applied_angle * factor;
}

void CL_ClearView(void)
{
	memset(&fade, 0, sizeof(fade));
	memset(&shake, 0, sizeof(shake));
	for (int i = 0; i < NETMESSAGES; i++)
		netmsg[i].pName = NULL;
}

void CL_InitView(void)
{
	CL_ClearView();
}
