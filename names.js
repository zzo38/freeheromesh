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
console.log("// Auto-generated! Do not modify directly!");
standard_message_names.forEach(([a,b,c])=>console.log("#define MSG_"+c+" "+b));
console.log("static const char*const standard_message_names[]={");
standard_message_names.forEach(([a,b,c])=>console.log(" \""+c+"\","));
console.log("};");
standard_sound_names.forEach((x,y)=>console.log("#define SND_"+x+" "+y));
console.log("static const char*const standard_sound_names[]={");
standard_sound_names.forEach(x=>console.log(" \""+x+"\","));
console.log("};");
