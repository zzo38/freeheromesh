// Program to generate names.h file.
// Public domain.
"use strict";
const standard_message_names=`
 // Original
 0 = INIT
 1 = CREATE
 2 = DESTROY
 3 = BEGIN_TURN
 4 = ARRIVED
 5 = DEPARTED
 6 = LASTIMAGE
 7 = MOVED
 8 = JUMPED
 9 = KEY
 10 = MOVING
 11 = SUNK
 12 = FLOATED
 13 = PLAYERMOVING
 14 = HIT
 15 = HITBY
 16 = DESTROYED
 17 = CREATED
 18 = POSTINIT
 19 = END_TURN
 // New
 20 = CLEANUP
 21 = COLLIDING
 22 = COLLIDE
 23 = BIZARRO_SWAP
`.split("\n").map(x=>/^ *([0-9]+) = ([^ ]*) *$/.exec(x)).filter(x=>x);
const standard_sound_names=[];
`
 SPLASH
 POUR
 DOOR
 GLASS
 BANG
 UNHH
 UH_OH
 FROG
 THWIT
 KLINKK
 POWER
 KLECK
 CLICK
 SMALL_POP
 DINK
 TICK
 CHYEW
 CHEEP
 ANHH
 BRRRT
 BRRREEET
 BWEEP
 DRLRLRINK
 FFFFTT
 WAHOO
 YEEHAW
 OLDPHONE
 RATTLE
 BEEDEEP
 THMP_thmp
 BEDOINGNG
 HEARTBEAT
 LOCK
 TAHTASHH
 BOOOM
 VACUUM
 RATCHET2
 DYUPE
 UNCORK
 BOUNCE
 JAYAYAYNG
 DEEP_POP
 RATCHET1
 GLISSANT
 BUZZER
 FAROUT
 KEWEL
 WHACK
 STEAM
 HAWK
`.replace(/[A-Za-z_0-9]+/g,x=>standard_sound_names.push(x));
const heromesh_key_names=Object.create(null);
`
   8  BACK
   9  TAB
  12  CENTER
  13  ENTER
  16  SHIFT
  17  CTRL
  19  BREAK
  20  CAPSLOCK
  32  SPACE
  33  PGUP
  34  PGDN
  35  END
  36  HOME
  37  LEFT
  38  UP
  39  RIGHT
  40  DOWN
  46  DELETE
  96  NUMPAD0
  97  NUMPAD1
  98  NUMPAD2
  99  NUMPAD3
 100  NUMPAD4
 101  NUMPAD5
 102  NUMPAD6
 103  NUMPAD7
 104  NUMPAD8
 105  NUMPAD9
 106  MULTIPLY
 110  DECIMAL
 111  DIVIDE
 120  F9
 121  F10
 122  F11
 123  F12
 144  NUMLOCK
 145  SCRLOCK
 186  SEMICOLON
 187  EQUALS
 188  COMMA
 189  MINUS
 190  PERIOD
 191  SLASH
 192  TILDE
 219  OBRACKET
 220  BACKSLASH
 221  CBRACKET
 222  QUOTE
`.replace(/([0-9]+) +([A-Z][A-Z0-9_]*)/g,(x,y,z)=>{
  heromesh_key_names[y]=z;
});
[..."ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"].forEach(x=>{
  heromesh_key_names[x.charCodeAt()]=x;
});
console.log("// Auto-generated! Do not modify directly!");
standard_message_names.forEach(([a,b,c])=>console.log("#define MSG_"+c+" "+b));
console.log("const char*const standard_message_names[]={");
standard_message_names.forEach(([a,b,c])=>console.log(" \""+c+"\","));
console.log("};");
standard_sound_names.forEach((x,y)=>console.log("#define SND_"+x+" "+y));
console.log("const char*const standard_sound_names[]={");
standard_sound_names.forEach(x=>console.log(" \""+x+"\","));
console.log("};");
console.log("const char*const heromesh_key_names[256]={");
Object.keys(heromesh_key_names).forEach(x=>console.log(" ["+x+"]=\""+heromesh_key_names[x]+"\","));
console.log("};");
