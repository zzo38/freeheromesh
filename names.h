// Auto-generated! Do not modify directly!
#define MSG_INIT 0
#define MSG_CREATE 1
#define MSG_DESTROY 2
#define MSG_BEGIN_TURN 3
#define MSG_ARRIVED 4
#define MSG_DEPARTED 5
#define MSG_LASTIMAGE 6
#define MSG_MOVED 7
#define MSG_JUMPED 8
#define MSG_KEY 9
#define MSG_MOVING 10
#define MSG_SUNK 11
#define MSG_FLOATED 12
#define MSG_PLAYERMOVING 13
#define MSG_HIT 14
#define MSG_HITBY 15
#define MSG_DESTROYED 16
#define MSG_CREATED 17
#define MSG_POSTINIT 18
#define MSG_END_TURN 19
#define MSG_CLEANUP 20
#define MSG_COLLIDING 21
#define MSG_COLLIDE 22
#define MSG_BIZARRO_SWAP 23
#ifdef HEROMESH_MAIN
const char*const standard_message_names[]={
 "INIT",
 "CREATE",
 "DESTROY",
 "BEGIN_TURN",
 "ARRIVED",
 "DEPARTED",
 "LASTIMAGE",
 "MOVED",
 "JUMPED",
 "KEY",
 "MOVING",
 "SUNK",
 "FLOATED",
 "PLAYERMOVING",
 "HIT",
 "HITBY",
 "DESTROYED",
 "CREATED",
 "POSTINIT",
 "END_TURN",
 "CLEANUP",
 "COLLIDING",
 "COLLIDE",
 "BIZARRO_SWAP",
};
#endif
#define SND_SPLASH 0
#define SND_POUR 1
#define SND_DOOR 2
#define SND_GLASS 3
#define SND_BANG 4
#define SND_UNHH 5
#define SND_UH_OH 6
#define SND_FROG 7
#define SND_THWIT 8
#define SND_KLINKK 9
#define SND_POWER 10
#define SND_KLECK 11
#define SND_CLICK 12
#define SND_SMALL_POP 13
#define SND_DINK 14
#define SND_TICK 15
#define SND_CHYEW 16
#define SND_CHEEP 17
#define SND_ANHH 18
#define SND_BRRRT 19
#define SND_BRRREEET 20
#define SND_BWEEP 21
#define SND_DRLRLRINK 22
#define SND_FFFFTT 23
#define SND_WAHOO 24
#define SND_YEEHAW 25
#define SND_OLDPHONE 26
#define SND_RATTLE 27
#define SND_BEEDEEP 28
#define SND_THMP_thmp 29
#define SND_BEDOINGNG 30
#define SND_HEARTBEAT 31
#define SND_LOCK 32
#define SND_TAHTASHH 33
#define SND_BOOOM 34
#define SND_VACUUM 35
#define SND_RATCHET2 36
#define SND_DYUPE 37
#define SND_UNCORK 38
#define SND_BOUNCE 39
#define SND_JAYAYAYNG 40
#define SND_DEEP_POP 41
#define SND_RATCHET1 42
#define SND_GLISSANT 43
#define SND_BUZZER 44
#define SND_FAROUT 45
#define SND_KEWEL 46
#define SND_WHACK 47
#define SND_STEAM 48
#define SND_HAWK 49
#ifdef HEROMESH_MAIN
const char*const standard_sound_names[]={
 "SPLASH",
 "POUR",
 "DOOR",
 "GLASS",
 "BANG",
 "UNHH",
 "UH_OH",
 "FROG",
 "THWIT",
 "KLINKK",
 "POWER",
 "KLECK",
 "CLICK",
 "SMALL_POP",
 "DINK",
 "TICK",
 "CHYEW",
 "CHEEP",
 "ANHH",
 "BRRRT",
 "BRRREEET",
 "BWEEP",
 "DRLRLRINK",
 "FFFFTT",
 "WAHOO",
 "YEEHAW",
 "OLDPHONE",
 "RATTLE",
 "BEEDEEP",
 "THMP_thmp",
 "BEDOINGNG",
 "HEARTBEAT",
 "LOCK",
 "TAHTASHH",
 "BOOOM",
 "VACUUM",
 "RATCHET2",
 "DYUPE",
 "UNCORK",
 "BOUNCE",
 "JAYAYAYNG",
 "DEEP_POP",
 "RATCHET1",
 "GLISSANT",
 "BUZZER",
 "FAROUT",
 "KEWEL",
 "WHACK",
 "STEAM",
 "HAWK",
};
const char*const heromesh_key_names[256]={
 [8]="BACK",
 [9]="TAB",
 [12]="CENTER",
 [13]="ENTER",
 [16]="SHIFT",
 [17]="CTRL",
 [19]="BREAK",
 [20]="CAPSLOCK",
 [32]="SPACE",
 [33]="PGUP",
 [34]="PGDN",
 [35]="END",
 [36]="HOME",
 [37]="LEFT",
 [38]="UP",
 [39]="RIGHT",
 [40]="DOWN",
 [46]="DELETE",
 [48]="0",
 [49]="1",
 [50]="2",
 [51]="3",
 [52]="4",
 [53]="5",
 [54]="6",
 [55]="7",
 [56]="8",
 [57]="9",
 [65]="A",
 [66]="B",
 [67]="C",
 [68]="D",
 [69]="E",
 [70]="F",
 [71]="G",
 [72]="H",
 [73]="I",
 [74]="J",
 [75]="K",
 [76]="L",
 [77]="M",
 [78]="N",
 [79]="O",
 [80]="P",
 [81]="Q",
 [82]="R",
 [83]="S",
 [84]="T",
 [85]="U",
 [86]="V",
 [87]="W",
 [88]="X",
 [89]="Y",
 [90]="Z",
 [96]="NUMPAD0",
 [97]="NUMPAD1",
 [98]="NUMPAD2",
 [99]="NUMPAD3",
 [100]="NUMPAD4",
 [101]="NUMPAD5",
 [102]="NUMPAD6",
 [103]="NUMPAD7",
 [104]="NUMPAD8",
 [105]="NUMPAD9",
 [106]="MULTIPLY",
 [110]="DECIMAL",
 [111]="DIVIDE",
 [120]="F9",
 [121]="F10",
 [122]="F11",
 [123]="F12",
 [144]="NUMLOCK",
 [145]="SCRLOCK",
 [186]="SEMICOLON",
 [187]="EQUALS",
 [188]="COMMA",
 [189]="MINUS",
 [190]="PERIOD",
 [191]="SLASH",
 [192]="TILDE",
 [219]="OBRACKET",
 [220]="BACKSLASH",
 [221]="CBRACKET",
 [222]="QUOTE",
};
#endif
