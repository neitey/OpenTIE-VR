/* Voice commands: phrase table and matcher; see voice_commands.h.
 * The matching rules are OpenXWA-VR's (src/xwa_app/voice_core.c), the
 * table is TIE Fighter's (src/tie_runtime/input/actions.h; the wingman
 * orders as user.c carries them out). */

#include "tie_app/quest/voice_commands.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct TvCommand {
	TieInputAction action;
	const char* label;      /* the confirmation */
	const char* phrases[8]; /* what can be said, lower case */
} TvCommand;

static const TvCommand k_commands[] = {
	/* Wingmen (Shift+letter). A, C, I go to the player's flight group;
	 * E, W, G, H, R to the targeted wingman (user.c user_checkradio). */
	{ TIE_INPUT_ACTION_COMM_ASSIGN_TARGET, "Wingmen: attack my target",
	  { "attack my target", "go after my target", "take out my target", "engage my target", "attack target",
		"attack" } },
	{ TIE_INPUT_ACTION_COMM_COVER_ME, "Wingmen: cover me",
	  { "cover me", "protect me", "watch my back", "cover my six", "help me", "defend me", "get him off me" } },
	{ TIE_INPUT_ACTION_COMM_IGNORE_TARGET, "Wingmen: ignore my target",
	  { "ignore my target", "ignore target", "ignore", "leave my target", "leave it alone", "forget my target" } },
	{ TIE_INPUT_ACTION_COMM_EVADE, "Wingman: evade",
	  { "evade", "evasive action", "evasive maneuvers", "break off", "break formation", "scatter" } },
	{ TIE_INPUT_ACTION_COMM_STOP_AND_WAIT, "Wingman: wait",
	  { "wait", "hold position", "stand by", "hold on", "stay here", "hold up", "stop and wait" } },
	{ TIE_INPUT_ACTION_COMM_CONTINUE_MISSION, "Wingman: go ahead",
	  { "go ahead", "proceed", "carry on", "continue", "proceed with mission", "continue mission" } },
	{ TIE_INPUT_ACTION_COMM_HEAD_HOME, "Wingman: head home",
	  { "return to base", "go home", "head home", "head back", "back to base", "return home" } },
	{ TIE_INPUT_ACTION_COMM_REPORT_ORDERS, "Wingman: report in",
	  { "report in", "status report", "report status", "sound off", "check in", "report", "report orders" } },
	{ TIE_INPUT_ACTION_COMM_REQUEST_RESUPPLY, "Call for resupply",
	  { "call resupply", "request resupply", "resupply", "need supplies", "call for supplies", "reload",
		"rearm" } },
	{ TIE_INPUT_ACTION_COMM_REINFORCEMENTS, "Call for reinforcements",
	  { "call reinforcements", "request reinforcements", "reinforcements", "need backup", "send backup",
		"call for backup" } },
	/* Targeting. */
	{ TIE_INPUT_ACTION_TARGET_NEAREST_FIGHTER_OR_MINE, "Target nearest enemy fighter",
	  { "target closest enemy", "closest enemy", "nearest enemy", "target nearest enemy", "nearest fighter",
		"enemy", "nearest enemy fighter", "target nearest fighter" } },
	{ TIE_INPUT_ACTION_TARGET_NEXT, "Next target",
	  { "next target", "target next", "cycle target", "another target", "next one" } },
	{ TIE_INPUT_ACTION_TARGET_PREV, "Previous target",
	  { "previous target", "last target", "target previous", "go back target" } },
	{ TIE_INPUT_ACTION_TARGET_MY_ATTACKER, "Target my attacker",
	  { "target my attacker", "my attacker", "who is attacking me", "who is shooting at me", "who is shooting me" } },
	{ TIE_INPUT_ACTION_TARGET_ATTACKER_CHAIN, "Target my target's attacker",
	  { "who is attacking my target", "target my target attacker", "my target attacker", "attacker of my target",
		"who is shooting my target", "who is shooting at my target", "target attacker of my target" } },
	{ TIE_INPUT_ACTION_TARGET_NEWEST_CRAFT, "Target newest craft",
	  { "newest target", "newest craft", "new arrival", "newest", "new target", "latest arrival",
		"target newest craft" } },
	{ TIE_INPUT_ACTION_TARGET_CLEAR, "Target cleared",
	  { "clear target", "cancel target", "no target", "drop target", "deselect target", "clear lock" } },
	{ TIE_INPUT_ACTION_AUTO_TARGET, "Target in sights",
	  { "target in sights", "target ahead", "target in front", "target under crosshair", "target in crosshair",
		"target in front of me" } },
	{ TIE_INPUT_ACTION_TARGET_COMPONENT_NEXT, "Next component",
	  { "next component", "target component", "component", "next subsystem", "target subsystem", "subsystem" } },
	{ TIE_INPUT_ACTION_TARGET_COMPONENT_PREV, "Previous component",
	  { "previous component", "last component", "previous subsystem", "last subsystem" } },
	/* Weapons. */
	{ TIE_INPUT_ACTION_CYCLE_WEAPON_GROUP, "Change weapon",
	  { "change weapon", "next weapon", "switch weapon", "cycle weapons", "cycle warheads", "change warhead",
		"switch warhead", "weapons" } },
	{ TIE_INPUT_ACTION_CYCLE_WEAPON_FIRING_MODE, "Firing mode",
	  { "link weapons", "fire link", "link fire", "fire mode", "link lasers", "change fire mode", "dual fire",
		"firing mode" } },
	{ TIE_INPUT_ACTION_TOGGLE_BEAM, "Beam weapon",
	  { "beam", "toggle beam", "beam weapon", "beam on", "beam off", "activate beam", "tractor beam",
		"jamming beam" } },
	/* Energy. */
	{ TIE_INPUT_ACTION_XFER_CANNON_TO_SHIELDS, "Lasers to shields",
	  { "lasers to shields", "transfer lasers to shields", "boost shields", "power to shields", "energy to shields",
		"transfer to shields" } },
	{ TIE_INPUT_ACTION_XFER_SHIELDS_TO_CANNON, "Shields to lasers",
	  { "shields to lasers", "transfer shields to lasers", "boost lasers", "power to lasers", "energy to lasers",
		"transfer to lasers" } },
	{ TIE_INPUT_ACTION_CYCLE_SHIELD_MODE, "Shield direction",
	  { "shield direction", "shields forward", "shields front", "shields aft", "shields rear", "balance shields",
		"shield mode", "angle shields" } },
	{ TIE_INPUT_ACTION_CYCLE_CANNON_RECHARGE_RATE, "Laser recharge rate",
	  { "laser recharge rate", "laser recharge", "laser rate", "recharge lasers", "cannon recharge rate",
		"laser power" } },
	{ TIE_INPUT_ACTION_CYCLE_SHIELD_RECHARGE_RATE, "Shield recharge rate",
	  { "shield recharge rate", "shield recharge", "shield rate", "recharge shields", "shield power" } },
	{ TIE_INPUT_ACTION_CYCLE_BEAM_RECHARGE_RATE, "Beam recharge rate",
	  { "beam recharge rate", "beam recharge", "beam rate", "recharge beam", "beam power" } },
	/* Throttle ("speed" is heard as "throttle"). */
	{ TIE_INPUT_ACTION_THROTTLE_FULL, "Full throttle",
	  { "full throttle", "full speed", "maximum throttle", "max throttle", "throttle full", "hit it",
		"maximum speed" } },
	{ TIE_INPUT_ACTION_THROTTLE_ZERO, "Zero throttle",
	  { "zero throttle", "full stop", "stop", "all stop", "throttle zero", "kill throttle", "stop engines",
		"cut throttle" } },
	{ TIE_INPUT_ACTION_THROTTLE_ONE_THIRD, "One third throttle",
	  { "one third throttle", "one third", "throttle one third", "third" } },
	{ TIE_INPUT_ACTION_THROTTLE_TWO_THIRDS, "Two thirds throttle",
	  { "two thirds throttle", "two thirds", "throttle two thirds", "two third" } },
	{ TIE_INPUT_ACTION_MATCH_SPEED, "Match speed",
	  { "match speed", "match target speed", "match their speed", "match velocity", "match it" } },
	{ TIE_INPUT_ACTION_TOGGLE_SLAM, "SLAM overdrive",
	  { "slam", "toggle slam", "overdrive", "engage slam", "slam on", "slam off" } },
	/* Information. */
	{ TIE_INPUT_ACTION_INFO_GOALS, "Mission goals",
	  { "goals", "mission goals", "show goals", "objectives", "mission objectives", "show objectives" } },
	{ TIE_INPUT_ACTION_INFO_MAP, "Map", { "map", "open map", "show map", "tactical map", "map view" } },
	{ TIE_INPUT_ACTION_INFO_DAMAGE, "Damage report",
	  { "damage report", "damage", "show damage", "damage control", "ship damage", "repairs" } },
	{ TIE_INPUT_ACTION_INFO_MESSAGES, "Message log",
	  { "messages", "message log", "show messages", "radio log", "read messages", "log" } },
	{ TIE_INPUT_ACTION_INFO_WINGMAN_COMMANDS, "Wingman commands",
	  { "commands", "show commands", "list commands", "command list", "orders list" } },
	/* Ship and mission. */
	{ TIE_INPUT_ACTION_HYPERSPACE, "Hyperspace",
	  { "hyperspace", "jump to hyperspace", "make the jump", "punch it", "engage hyperdrive", "light speed",
		"hyperdrive" } },
	{ TIE_INPUT_ACTION_PAUSE, "Pause", { "pause", "pause game", "resume", "unpause", "resume game" } },
	{ TIE_INPUT_ACTION_END_MISSION, "End mission", { "end mission", "abort mission", "quit mission" } },
	/* The game's "press Space" prompts (reinforcements, docking, the end of
	 * the mission). */
	{ TIE_INPUT_ACTION_CONFIRM, "Confirm", { "confirm", "accept", "affirmative", "acknowledge", "yes" } },
};

/* Words that settle which command it is: when one is heard, only commands
 * with that word in a phrase are considered ("beam recharge" is not the
 * beam on/off switch; "who is attacking my target" is not "attack my
 * target"). */
static const char* const k_marker_words[] = { "who", "attacker", "recharge", "beam", "third" };

/* Heard words that mean the same to us, and words that change nothing
 * ("please", "the", who is addressed). Applied to the phrases too. */
static const char* const k_same[][2] = {
	{ "closest", "nearest" },     { "near", "nearest" },         { "nearby", "nearest" },
	{ "hostile", "enemy" },       { "hostiles", "enemy" },       { "bandit", "enemy" },
	{ "bandits", "enemy" },       { "enemies", "enemy" },        { "foe", "enemy" },
	{ "rebel", "enemy" },         { "rebels", "enemy" },         { "fighters", "fighter" },
	{ "targets", "target" },      { "attacking", "attack" },     { "attacks", "attack" },
	{ "engaging", "engage" },     { "laser", "lasers" },         { "cannon", "lasers" },
	{ "cannons", "lasers" },      { "guns", "lasers" },          { "shield", "shields" },
	{ "deflector", "shields" },   { "deflectors", "shields" },   { "prior", "previous" },
	{ "latest", "newest" },       { "goal", "goals" },           { "objective", "objectives" },
	{ "subsystem", "component" }, { "subsystems", "component" }, { "components", "component" },
	{ "part", "component" },      { "reinforcement", "reinforcements" },
	{ "backups", "backup" },      { "supply", "supplies" },      { "maneuver", "maneuvers" },
	{ "manoeuvres", "maneuvers" }, { "rtb", "return to base" },  { "whos", "who is" },
	{ "who's", "who is" },        { "its", "it" },               { "speed", "throttle" },
	{ "speeds", "throttle" },     { "velocity", "throttle" },    { "target's", "target" },
	{ "message", "messages" },    { "thirds", "third" },         { "too", "two" },
	{ "recharging", "recharge" }, { "weapon", "weapons" },       { "warheads", "warhead" },
	{ "missiles", "warhead" },    { "missile", "warhead" },      { "torpedoes", "warhead" },
	{ "torpedo", "warhead" },     { "beams", "beam" },
	{ "lightspeed", "light speed" }, { "sights", "crosshair" },   { "sight", "crosshair" },
	{ "crosshairs", "crosshair" }, { "reticle", "crosshair" },
};

static const char* const k_filler[] = {
	"please",  "now",     "the",       "a",     "an",    "uh",     "um",     "okay",   "ok",   "hey",
	"computer", "all",    "guys",      "you",   "everyone", "team", "squad", "flight", "wing", "wingmen",
	"wingman", "wingmates", "group",   "lead",  "and",   "just",   "that",   "this",   "go",   "be",
	"can",     "will",    "would",     "could", "for",   "to",     "into",   "s",      "shoot", "deploy",
	"put",     "set",     "give",      "some",
};
#define TV_COMMANDS ((int)(sizeof k_commands / sizeof k_commands[0]))
#define TV_MAX_WORDS 12
#define TV_WORD 24

int TieVoiceCommands_Count(void) { return TV_COMMANDS; }

TieInputAction TieVoiceCommands_Action(int index) {
	return index >= 0 && index < TV_COMMANDS ? k_commands[index].action : TIE_INPUT_ACTION_NONE;
}

const char* TieVoiceCommands_Label(int index) {
	return index >= 0 && index < TV_COMMANDS ? k_commands[index].label : NULL;
}

const char* TieVoiceCommands_Phrase(int index, int phrase) {
	if (index < 0 || index >= TV_COMMANDS || phrase < 0 || phrase >= 8)
		return NULL;
	return k_commands[index].phrases[phrase];
}

/* Adds `word` (after the synonyms; fillers dropped) to `words`. */
static int TieVoiceCommands_AddWord(char words[TV_MAX_WORDS][TV_WORD], int n, const char* word, int depth) {
	for (size_t i = 0; i < sizeof k_filler / sizeof k_filler[0]; ++i)
		if (strcmp(word, k_filler[i]) == 0)
			return n;
	for (size_t i = 0; depth < 2 && i < sizeof k_same / sizeof k_same[0]; ++i) {
		if (strcmp(word, k_same[i][0]) == 0) {
			/* The replacement may be several words ("rtb"); they are
			 * folded in turn, as the phrases' words are. */
			const char* r = k_same[i][1];
			while (*r && n < TV_MAX_WORDS) {
				char part[TV_WORD];
				int len = 0;
				while (r[len] && r[len] != ' ')
					++len;
				snprintf(part, sizeof part, "%.*s", len, r);
				n = TieVoiceCommands_AddWord(words, n, part, depth + 1);
				r += len;
				while (*r == ' ')
					++r;
			}
			return n;
		}
	}
	if (n < TV_MAX_WORDS)
		snprintf(words[n++], TV_WORD, "%s", word);
	return n;
}

/* Words of `text`: lower case letters, digits and apostrophes ("who's").
 * Synonyms are folded, fillers dropped. "to third(s)" is "two thirds" (the
 * recognizer often spells "two" as "to", which is otherwise a filler). */
static int TieVoiceCommands_Words(const char* text, char words[TV_MAX_WORDS][TV_WORD]) {
	int n = 0, len = 0;
	char word[TV_WORD];
	char previous[TV_WORD] = "";
	for (const char* c = text;; ++c) {
		const int letter = *c && (isalnum((unsigned char)*c) || *c == '\'');
		if (letter && len < TV_WORD - 1) {
			word[len++] = (char)tolower((unsigned char)*c);
		} else if (!letter && len > 0) {
			word[len] = '\0';
			len = 0;
			if (strcmp(previous, "to") == 0 && strncmp(word, "third", 5) == 0)
				n = TieVoiceCommands_AddWord(words, n, "two", 0);
			if (n < TV_MAX_WORDS)
				n = TieVoiceCommands_AddWord(words, n, word, 0);
			snprintf(previous, sizeof previous, "%s", word);
			if (n >= TV_MAX_WORDS)
				break;
		}
		if (!*c)
			break;
	}
	return n;
}

/* A word joined from two heard ones ("sub system"), folded like a heard
 * word when it has a one-word synonym. */
static const char* TieVoiceCommands_Fold(const char* word) {
	for (size_t i = 0; i < sizeof k_same / sizeof k_same[0]; ++i)
		if (strcmp(word, k_same[i][0]) == 0 && !strchr(k_same[i][1], ' '))
			return k_same[i][1];
	return word;
}

/* 0 (same) .. 1 (nothing alike): edit distance over the longer length. */
static float TieVoiceCommands_WordCost(const char* a, const char* b) {
	const int la = (int)strlen(a), lb = (int)strlen(b);
	int d[TV_WORD][TV_WORD];
	if (la >= TV_WORD || lb >= TV_WORD)
		return 1.0f;
	for (int i = 0; i <= la; ++i)
		d[i][0] = i;
	for (int j = 0; j <= lb; ++j)
		d[0][j] = j;
	for (int i = 1; i <= la; ++i) {
		for (int j = 1; j <= lb; ++j) {
			const int sub = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
			const int del = d[i - 1][j] + 1, ins = d[i][j - 1] + 1;
			d[i][j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
		}
	}
	const int m = la > lb ? la : lb;
	return m ? (float)d[la][lb] / (float)m : 0.0f;
}

/* Alignment cost of the heard words against a phrase: a word swapped for a
 * look-alike costs its spelling distance, a missing phrase word 1, an extra
 * heard word 0.6 (0.15 before the phrase, 0.25 after: "uh", "please").
 * Normalised by the phrase length. */
static float TieVoiceCommands_PhraseCost(char heard[TV_MAX_WORDS][TV_WORD], int nh, const char* phrase) {
	char p[TV_MAX_WORDS][TV_WORD];
	const int np = TieVoiceCommands_Words(phrase, p);
	if (np == 0)
		return 9.0f;
	float d[TV_MAX_WORDS + 1][TV_MAX_WORDS + 1];
	for (int i = 0; i <= nh; ++i)
		d[i][0] = 0.15f * (float)i; /* leading extra words ("uh", "so") */
	for (int j = 1; j <= np; ++j)
		d[0][j] = (float)j;
	for (int i = 1; i <= nh; ++i) {
		for (int j = 1; j <= np; ++j) {
			const float c = TieVoiceCommands_WordCost(heard[i - 1], p[j - 1]);
			const float sub = d[i - 1][j - 1] + (c < 0.5f ? c : 1.0f);
			const float extra = d[i - 1][j] + (j == np ? 0.25f : 0.6f);
			const float missing = d[i][j - 1] + 1.0f;
			float best = sub < extra ? sub : extra;
			best = best < missing ? best : missing;
			/* Two heard words for one phrase word ("hyper space"). */
			if (i >= 2) {
				char joined[2 * TV_WORD];
				snprintf(joined, sizeof joined, "%s%s", heard[i - 2], heard[i - 1]);
				if (strlen(joined) < TV_WORD) {
					const float cj = TieVoiceCommands_WordCost(TieVoiceCommands_Fold(joined), p[j - 1]);
					const float merged = d[i - 2][j - 1] + (cj < 0.5f ? cj : 1.0f) + 0.1f;
					best = best < merged ? best : merged;
				}
			}
			d[i][j] = best;
		}
	}
	return d[nh][np] / (float)np;
}

int TieVoiceCommands_Match(const char* heard, TieInputAction* action, const char** label) {
	char words[TV_MAX_WORDS][TV_WORD];
	const int nh = heard ? TieVoiceCommands_Words(heard, words) : 0;
	if (nh == 0)
		return 0;
	/* A marker word heard (or close to it): only commands that use it. */
	const char* marker = NULL;
	for (size_t m = 0; m < sizeof k_marker_words / sizeof k_marker_words[0] && !marker; ++m) {
		for (int w = 0; w < nh; ++w) {
			if (TieVoiceCommands_WordCost(words[w], k_marker_words[m]) <= 0.2f) {
				marker = k_marker_words[m];
				break;
			}
		}
	}
	float cost_of[TV_COMMANDS];
	float best = 9.0f;
	int best_i = -1;
	for (int i = 0; i < TV_COMMANDS; ++i) {
		float cost = 9.0f;
		int has_marker = marker == NULL;
		for (int k = 0; k < 8 && k_commands[i].phrases[k]; ++k) {
			const float c = TieVoiceCommands_PhraseCost(words, nh, k_commands[i].phrases[k]);
			cost = c < cost ? c : cost;
			if (marker && strstr(k_commands[i].phrases[k], marker))
				has_marker = 1;
		}
		cost_of[i] = has_marker ? cost : 9.0f;
		if (cost_of[i] < best) {
			best = cost_of[i];
			best_i = i;
		}
	}
	/* The runner-up among the other actions. */
	float second = 9.0f;
	for (int i = 0; i < TV_COMMANDS; ++i)
		if (best_i >= 0 && k_commands[i].action != k_commands[best_i].action && cost_of[i] < second)
			second = cost_of[i];
	/* Close enough, and clearly closer than any other command. */
	if (best_i < 0 || best > 0.34f || second - best < 0.12f)
		return 0;
	if (action)
		*action = k_commands[best_i].action;
	if (label)
		*label = k_commands[best_i].label;
	return 1;
}
