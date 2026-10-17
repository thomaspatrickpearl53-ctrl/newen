// newen - movement input: +forward, +jump and friends, turned into a usercmd every frame
//
// In GoldSrc these commands belong to client.dll. Until step 5 loads it, the engine provides them so
// the player can move; they are named and bound the same way, and go away when the client DLL
// registers its own.
#include "server.h"

vec3_t cl_viewangles;   // where the player is looking; the mouse moves it

typedef struct {
	int down[2];        // keys holding it down (two can)
	qboolean active;
} kbutton_t;

static kbutton_t in_forward, in_back, in_moveleft, in_moveright, in_moveup, in_movedown;
static kbutton_t in_jump, in_duck, in_attack, in_attack2, in_use, in_reload, in_speed, in_alt1, in_score;
static int pending_impulse;
static cvar_t *cl_forwardspeed, *cl_backspeed, *cl_sidespeed, *cl_upspeed, *cl_movespeedkey;

static void KeyDown(kbutton_t *b)
{
	int k = Cmd_Argc() > 1 ? atoi(Cmd_Argv(1)) : -1;   // bindings append the key number
	if (k == b->down[0] || k == b->down[1])
		return;
	if (!b->down[0])
		b->down[0] = k;
	else if (!b->down[1])
		b->down[1] = k;
	else
		return;   // three keys for one button
	b->active = true;
}

static void KeyUp(kbutton_t *b)
{
	if (Cmd_Argc() < 2) {
		b->down[0] = b->down[1] = 0;   // typed in the console: release everything
		b->active = false;
		return;
	}
	int k = atoi(Cmd_Argv(1));
	if (b->down[0] == k)
		b->down[0] = 0;
	else if (b->down[1] == k)
		b->down[1] = 0;
	else
		return;
	if (!b->down[0] && !b->down[1])
		b->active = false;
}

#define BUTTON(name, var) \
	static void name##Down_f(void) { KeyDown(&var); } \
	static void name##Up_f(void) { KeyUp(&var); }
BUTTON(Forward, in_forward)
BUTTON(Back, in_back)
BUTTON(MoveLeft, in_moveleft)
BUTTON(MoveRight, in_moveright)
BUTTON(MoveUp, in_moveup)
BUTTON(MoveDown, in_movedown)
BUTTON(Jump, in_jump)
BUTTON(Duck, in_duck)
BUTTON(Attack, in_attack)
BUTTON(Attack2, in_attack2)
BUTTON(Use, in_use)
BUTTON(Reload, in_reload)
BUTTON(Speed, in_speed)
BUTTON(Alt1, in_alt1)
BUTTON(Score, in_score)

static void Impulse_f(void)
{
	if (Cmd_Argc() > 1)
		pending_impulse = atoi(Cmd_Argv(1));
}

// without a client DLL, the engine builds the command itself
void CL_CreateCmdEngine(float frametime, usercmd_t *cmd)
{
	memset(cmd, 0, sizeof(*cmd));
	int msec = (int)(frametime * 1000.0f + 0.5f);
	cmd->msec = (unsigned char)(msec < 1 ? 1 : msec > 255 ? 255 : msec);
	VectorCopy(cl_viewangles, cmd->viewangles);

	qboolean typing = Con_IsVisible();
	if (!typing) {
		cmd->forwardmove = cl_forwardspeed->value * in_forward.active - cl_backspeed->value * in_back.active;
		cmd->sidemove = cl_sidespeed->value * (in_moveright.active - in_moveleft.active);
		cmd->upmove = cl_upspeed->value * (in_moveup.active - in_movedown.active);
		if (in_speed.active) {
			cmd->forwardmove *= cl_movespeedkey->value;
			cmd->sidemove *= cl_movespeedkey->value;
			cmd->upmove *= cl_movespeedkey->value;
		}
		unsigned short b = 0;
		if (in_attack.active)    b |= IN_ATTACK;
		if (in_attack2.active)   b |= IN_ATTACK2;
		if (in_jump.active)      b |= IN_JUMP;
		if (in_duck.active)      b |= IN_DUCK;
		if (in_use.active)       b |= IN_USE;
		if (in_reload.active)    b |= IN_RELOAD;
		if (in_forward.active)   b |= IN_FORWARD;
		if (in_back.active)      b |= IN_BACK;
		if (in_moveleft.active)  b |= IN_MOVELEFT;
		if (in_moveright.active) b |= IN_MOVERIGHT;
		if (in_speed.active)     b |= IN_RUN;
		if (in_alt1.active)      b |= IN_ALT1;
		if (in_score.active)     b |= IN_SCORE;
		cmd->buttons = b;
	}
	cmd->impulse = (unsigned char)pending_impulse;
	pending_impulse = 0;
}

void CL_InitInput(void)
{
	cl_forwardspeed = Cvar_Get("cl_forwardspeed", "400", FCVAR_ARCHIVE, "forward speed (the server caps it at sv_maxspeed)");
	cl_backspeed = Cvar_Get("cl_backspeed", "400", FCVAR_ARCHIVE, "backward speed");
	cl_sidespeed = Cvar_Get("cl_sidespeed", "400", FCVAR_ARCHIVE, "strafing speed");
	cl_upspeed = Cvar_Get("cl_upspeed", "320", FCVAR_ARCHIVE, "swimming and noclip up/down speed");
	cl_movespeedkey = Cvar_Get("cl_movespeedkey", "0.3", FCVAR_ARCHIVE, "speed while +speed (walk) is held");
	static const struct { const char *name; xcommand_t down, up; } buttons[] = {
		{ "forward", ForwardDown_f, ForwardUp_f }, { "back", BackDown_f, BackUp_f },
		{ "moveleft", MoveLeftDown_f, MoveLeftUp_f }, { "moveright", MoveRightDown_f, MoveRightUp_f },
		{ "moveup", MoveUpDown_f, MoveUpUp_f }, { "movedown", MoveDownDown_f, MoveDownUp_f },
		{ "jump", JumpDown_f, JumpUp_f }, { "duck", DuckDown_f, DuckUp_f },
		{ "attack", AttackDown_f, AttackUp_f }, { "attack2", Attack2Down_f, Attack2Up_f },
		{ "use", UseDown_f, UseUp_f }, { "reload", ReloadDown_f, ReloadUp_f },
		{ "speed", SpeedDown_f, SpeedUp_f }, { "alt1", Alt1Down_f, Alt1Up_f },
		{ "showscores", ScoreDown_f, ScoreUp_f },
	};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		Cmd_AddCommand(va("+%s", buttons[i].name), buttons[i].down, NULL);
		Cmd_AddCommand(va("-%s", buttons[i].name), buttons[i].up, NULL);
	}
	Cmd_AddCommand("impulse", Impulse_f, "impulse <n> - sends an impulse to the game (101 gives everything with sv_cheats 1)");
}
